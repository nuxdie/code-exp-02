#include "system_theme.h"
#include <chrono>
#include <gio/gio.h>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

namespace {
unsigned preference = 1;
void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
// The GDBus callback ABI fixes the argument count.
// NOLINTBEGIN(readability-function-size)
void read_setting(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
                  GVariant*, GDBusMethodInvocation* invocation, gpointer)
{
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(v)", g_variant_new_uint32(preference)));
}
// NOLINTEND(readability-function-size)
void await_theme(SystemTheme& theme, bool dark)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_iteration(nullptr, false)) {
        }
        const auto value = theme.take_change();
        if (value && *value == dark)
            return;
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("System theme did not follow the portal preference");
}
void portal_roundtrip()
{
    auto* session = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(session);
    auto* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    expect(bus, "Private theme test bus must connect");
    auto* reply = g_dbus_connection_call_sync(
        bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", "org.freedesktop.portal.Desktop", 0), nullptr, G_DBUS_CALL_FLAGS_NONE,
        1000, nullptr, nullptr);
    expect(reply, "Test portal name must be acquired");
    g_variant_unref(reply);
    auto* info = g_dbus_node_info_new_for_xml(
        "<node><interface name='org.freedesktop.portal.Settings'><method name='Read'><arg type='s' "
        "direction='in'/><arg type='s' direction='in'/><arg type='v' "
        "direction='out'/></method><signal name='SettingChanged'><arg type='s'/><arg "
        "type='s'/><arg type='v'/></signal></interface></node>",
        nullptr);
    const GDBusInterfaceVTable table{read_setting, nullptr, nullptr, {nullptr}};
    const auto registration =
        g_dbus_connection_register_object(bus, "/org/freedesktop/portal/desktop",
                                          info->interfaces[0], &table, nullptr, nullptr, nullptr);
    expect(registration, "Test portal must expose Settings.Read");
    {
        SystemTheme theme;
        await_theme(theme, true);
        preference = 2;
        g_dbus_connection_emit_signal(bus, nullptr, "/org/freedesktop/portal/desktop",
                                      "org.freedesktop.portal.Settings", "SettingChanged",
                                      g_variant_new("(ssv)", "org.freedesktop.appearance",
                                                    "color-scheme",
                                                    g_variant_new_uint32(preference)),
                                      nullptr);
        await_theme(theme, false);
        preference = 1;
        g_dbus_connection_emit_signal(bus, nullptr, "/org/freedesktop/portal/desktop",
                                      "org.freedesktop.portal.Settings", "SettingChanged",
                                      g_variant_new("(ssv)", "org.freedesktop.appearance",
                                                    "color-scheme",
                                                    g_variant_new_uint32(preference)),
                                      nullptr);
        await_theme(theme, true);
    }
    g_dbus_connection_unregister_object(bus, registration);
    g_dbus_node_info_unref(info);
    g_object_unref(bus);
    g_test_dbus_down(session);
    g_object_unref(session);
}
} // namespace
int main()
{
    try {
        expect(theme_name_is_dark("Mint-Y-Dark-Sand") && theme_name_is_dark("Adwaita:dark") &&
                   !theme_name_is_dark("Adwaita"),
               "GTK theme names must preserve the desktop's dark variant");
        expect(!preferred_dark_theme(0) && preferred_dark_theme(1) == true &&
                   preferred_dark_theme(2) == false && !preferred_dark_theme(42),
               "Portal no-preference must remain distinct from explicit light");
        portal_roundtrip();
        std::cout << "System theme follows live portal changes in both directions.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
