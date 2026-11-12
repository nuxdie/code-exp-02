#include "system_theme.h"

#include "handles.h"
#include "text.h"
#include "wakeup.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <gio/gio.h>
#include <mutex>
#include <thread>
#include <utility>

bool theme_name_is_dark(std::string_view name)
{
    return lowercase(name).contains("dark");
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
using VariantHandle = Handle<GVariant, g_variant_unref>;

VariantHandle unbox(GVariant* reply)
{
    VariantHandle value{reply};
    while (value &&
           (g_variant_is_of_type(value.get(), G_VARIANT_TYPE_VARIANT) ||
            g_variant_is_of_type(value.get(), G_VARIANT_TYPE_TUPLE)) &&
           g_variant_n_children(value.get()))
        value.reset(g_variant_get_child_value(value.get(), 0));
    return value;
}
std::string setting(GSettings* settings, const char* key)
{
    const GStringHandle value{g_settings_get_string(settings, key)};
    return value.get();
}
} // namespace
struct SystemTheme::Impl {
    using MainContext = Handle<GMainContext, g_main_context_unref>;
    WakeSignal events;
    MainContext context{g_main_context_new()};
    std::mutex mutex;
    std::optional<bool> changed;
    std::optional<bool> current;
    GObjectHandle<GDBusConnection> bus;
    GObjectHandle<GSettings> settings;
    std::jthread worker;

    Impl()
        : worker([this](const std::stop_token& stopping) {
              const std::stop_callback wake(stopping,
                                            [this] { g_main_context_wakeup(context.get()); });
              run(stopping);
          })
    {
    }
    struct Call {
        const char *destination, *path, *interface, *method;
    };
    VariantHandle call(const Call& target, GVariant* params) const
    {
        if (!bus)
            return nullptr;
        return unbox(g_dbus_connection_call_sync(bus.get(), target.destination, target.path,
                                                 target.interface, target.method, params, nullptr,
                                                 G_DBUS_CALL_FLAGS_NONE, 1200, nullptr, nullptr));
    }
    std::optional<bool> portal_preference()
    {
        const auto value =
            call({"org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                  "org.freedesktop.portal.Settings", "Read"},
                 g_variant_new("(ss)", "org.freedesktop.appearance", "color-scheme"));
        if (value && g_variant_is_of_type(value.get(), G_VARIANT_TYPE_UINT32))
            return preferred_dark_theme(g_variant_get_uint32(value.get()));
        return std::nullopt;
    }
    bool fallback()
    {
        const auto value =
            call({"org.xfce.Xfconf", "/org/xfce/Xfconf", "org.xfce.Xfconf", "GetProperty"},
                 g_variant_new("(ss)", "xsettings", "/Net/ThemeName"));
        if (value && g_variant_is_of_type(value.get(), G_VARIANT_TYPE_STRING))
            return theme_name_is_dark(g_variant_get_string(value.get(), nullptr));
        if (settings) {
            const auto preference = setting(settings.get(), "color-scheme");
            if (preference == "prefer-dark" || preference == "prefer-light")
                return preference == "prefer-dark";
            return theme_name_is_dark(setting(settings.get(), "gtk-theme"));
        }
        const auto* name = std::getenv("GTK_THEME");
        return name && theme_name_is_dark(name);
    }
    void refresh()
    {
        const auto preference = portal_preference();
        const bool dark = preference ? *preference : fallback();
        if (current == dark)
            return;
        current = dark;
        {
            const std::scoped_lock lock(mutex);
            changed = dark;
        }
        events.raise();
    }
    // The GDBus callback ABI fixes the argument count.
    // NOLINTBEGIN(readability-function-size)
    static void signal(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
                       GVariant*, gpointer data)
    {
        static_cast<Impl*>(data)->refresh();
    }
    // NOLINTEND(readability-function-size)
    void watch_settings()
    {
        using Schema = Handle<GSettingsSchema, g_settings_schema_unref>;
        auto* source = g_settings_schema_source_get_default();
        const Schema schema{
            source ? g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", true)
                   : nullptr};
        if (!schema || !g_settings_schema_has_key(schema.get(), "color-scheme") ||
            !g_settings_schema_has_key(schema.get(), "gtk-theme"))
            return;
        settings.reset(g_settings_new_full(schema.get(), nullptr, nullptr));
        g_signal_connect(settings.get(), "changed",
                         G_CALLBACK(+[](GSettings*, gchar*, gpointer data) {
                             static_cast<Impl*>(data)->refresh();
                         }),
                         this);
    }
    void run(const std::stop_token& stopping)
    {
        g_main_context_push_thread_default(context.get());
        bus.reset(g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr));
        watch_settings();
        guint portal = 0, xfce = 0;
        if (bus) {
            portal = g_dbus_connection_signal_subscribe(
                bus.get(), "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Settings",
                "SettingChanged", "/org/freedesktop/portal/desktop", "org.freedesktop.appearance",
                G_DBUS_SIGNAL_FLAGS_NONE, signal, this, nullptr);
            xfce = g_dbus_connection_signal_subscribe(
                bus.get(), "org.xfce.Xfconf", "org.xfce.Xfconf", "PropertyChanged", nullptr,
                "xsettings", G_DBUS_SIGNAL_FLAGS_NONE, signal, this, nullptr);
        }
        refresh();
        while (!stopping.stop_requested())
            g_main_context_iteration(context.get(), true);
        if (bus) {
            g_dbus_connection_signal_unsubscribe(bus.get(), portal);
            g_dbus_connection_signal_unsubscribe(bus.get(), xfce);
        }
        bus.reset();
        settings.reset();
        g_main_context_pop_thread_default(context.get());
    }
};
SystemTheme::SystemTheme() : impl(std::make_unique<Impl>()) {}
SystemTheme::~SystemTheme() = default;
int SystemTheme::event_fd() const
{
    return impl->events.fd();
}
std::optional<bool> SystemTheme::take_change()
{
    impl->events.clear();
    const std::scoped_lock lock(impl->mutex);
    return std::exchange(impl->changed, std::nullopt);
}
