#include "system_theme.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <gio/gio.h>
#include <mutex>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

bool theme_name_is_dark(const std::string& name)
{
    std::string lowered = name;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
    return lowered.find("dark") != std::string::npos;
}
std::optional<bool> preferred_dark_theme(unsigned preference)
{
    if (preference == 1)
        return true;
    if (preference == 2)
        return false;
    return std::nullopt;
}
namespace {
GVariant* unbox(GVariant* value)
{
    while (value && (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT) ||
                     g_variant_is_of_type(value, G_VARIANT_TYPE_TUPLE))) {
        if (!g_variant_n_children(value))
            break;
        auto* next = g_variant_get_child_value(value, 0);
        g_variant_unref(value);
        value = next;
    }
    return value;
}
} // namespace
struct SystemTheme::Impl {
    int events = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    GMainContext* context = g_main_context_new();
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::optional<bool> changed;
    std::optional<bool> current;
    GDBusConnection* bus = nullptr;
    GSettings* settings = nullptr;
    std::thread worker;

    Impl() : worker([this] { run(); }) {}
    GVariant* call(const char* destination, const char* path, const char* interface,
                   const char* method, GVariant* params)
    {
        if (!bus)
            return nullptr;
        return unbox(g_dbus_connection_call_sync(bus, destination, path, interface, method, params,
                                                 nullptr, G_DBUS_CALL_FLAGS_NONE, 1200, nullptr,
                                                 nullptr));
    }
    std::optional<bool> portal_preference()
    {
        auto* value = call("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                           "org.freedesktop.portal.Settings", "Read",
                           g_variant_new("(ss)", "org.freedesktop.appearance", "color-scheme"));
        std::optional<bool> result;
        if (value && g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32))
            result = preferred_dark_theme(g_variant_get_uint32(value));
        if (value)
            g_variant_unref(value);
        return result;
    }
    bool fallback()
    {
        // Xfce exposes its actual current GTK theme through xfconf.
        auto* value = call("org.xfce.Xfconf", "/org/xfce/Xfconf", "org.xfce.Xfconf", "GetProperty",
                           g_variant_new("(ss)", "xsettings", "/Net/ThemeName"));
        if (value && g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
            const bool result = theme_name_is_dark(g_variant_get_string(value, nullptr));
            g_variant_unref(value);
            return result;
        }
        if (value)
            g_variant_unref(value);
        if (settings) {
            auto* scheme = g_settings_get_string(settings, "color-scheme");
            const std::string preference = scheme;
            g_free(scheme);
            if (preference == "prefer-dark" || preference == "prefer-light")
                return preference == "prefer-dark";
            auto* name = g_settings_get_string(settings, "gtk-theme");
            const bool result = theme_name_is_dark(name);
            g_free(name);
            return result;
        }
        const auto* name = std::getenv("GTK_THEME");
        return name && theme_name_is_dark(name);
    }
    void refresh()
    {
        const auto preference = portal_preference();
        const bool dark = preference ? *preference : fallback();
        if (current && *current == dark)
            return;
        current = dark;
        {
            std::lock_guard<std::mutex> lock(mutex);
            changed = dark;
        }
        const uint64_t wake = 1;
        (void)!write(events, &wake, sizeof(wake));
    }
    // The GDBus callback ABI fixes the argument count.
    // NOLINTBEGIN(readability-function-size)
    static void signal(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
                       GVariant*, gpointer data)
    {
        static_cast<Impl*>(data)->refresh();
    }
    // NOLINTEND(readability-function-size)
    void run()
    {
        g_main_context_push_thread_default(context);
        bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
        auto* source = g_settings_schema_source_get_default();
        auto* schema =
            source ? g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", true)
                   : nullptr;
        if (schema) {
            if (g_settings_schema_has_key(schema, "color-scheme") &&
                g_settings_schema_has_key(schema, "gtk-theme")) {
                settings = g_settings_new_full(schema, nullptr, nullptr);
                g_signal_connect(settings, "changed",
                                 G_CALLBACK(+[](GSettings*, gchar*, gpointer data) {
                                     static_cast<Impl*>(data)->refresh();
                                 }),
                                 this);
            }
            g_settings_schema_unref(schema);
        }
        guint portal = 0, xfce = 0;
        if (bus) {
            portal = g_dbus_connection_signal_subscribe(
                bus, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Settings",
                "SettingChanged", "/org/freedesktop/portal/desktop", "org.freedesktop.appearance",
                G_DBUS_SIGNAL_FLAGS_NONE, signal, this, nullptr);
            xfce = g_dbus_connection_signal_subscribe(
                bus, "org.xfce.Xfconf", "org.xfce.Xfconf", "PropertyChanged", nullptr, "xsettings",
                G_DBUS_SIGNAL_FLAGS_NONE, signal, this, nullptr);
        }
        refresh();
        while (!stopping)
            g_main_context_iteration(context, true);
        if (bus) {
            g_dbus_connection_signal_unsubscribe(bus, portal);
            g_dbus_connection_signal_unsubscribe(bus, xfce);
            g_object_unref(bus);
        }
        if (settings)
            g_object_unref(settings);
        g_main_context_pop_thread_default(context);
    }
    ~Impl()
    {
        stopping = true;
        g_main_context_wakeup(context);
        worker.join();
        g_main_context_unref(context);
        close(events);
    }
};
SystemTheme::SystemTheme() : impl(std::make_unique<Impl>()) {}
SystemTheme::~SystemTheme() = default;
int SystemTheme::event_fd() const
{
    return impl->events;
}
std::optional<bool> SystemTheme::take_change()
{
    uint64_t event;
    while (read(impl->events, &event, sizeof(event)) > 0) {
    }
    std::lock_guard<std::mutex> lock(impl->mutex);
    auto result = impl->changed;
    impl->changed.reset();
    return result;
}
