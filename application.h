#pragma once
#include "agent_client.h"
#include "chat_canvas.h"
#include "conversation.h"
#include "document_view.h"
#include "intent_canvas.h"
#include "live_build.h"
#include "math_typesetter.h"
#include "popup_markup.h"
#include "project_watch.h"
#include "source_graph.h"
#include "source_panel.h"
#include "syntax.h"
#include "system_theme.h"
#include "text_input.h"
#include "viewer_config.h"
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fontconfig/fontconfig.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <pango/pangocairo.h>
#include <poll.h>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <vector>

class Application {
    friend struct ApplicationTestAccess;

  public:
    static constexpr int reload_exit_code = 75;
    int run();
    ~Application();

  private:
    struct JumpRow {
        std::string label;
        lsp::Json location;
        SymbolNode* node = nullptr;
        int depth = 0;
    };

    using Clock = std::chrono::steady_clock;
    bool benchmark = false;
    Display* display = nullptr;
    std::filesystem::path executable;
    std::string workspace_root = project_root;
    std::unique_ptr<ProjectWatcher> project_watcher;
    int screen = 0;
    std::unique_ptr<ChatCanvas> chat;
    std::unique_ptr<IntentCanvas> intent;
    lsp::Json intent_session = lsp::Json::object();
    std::unique_ptr<SystemTheme> system_theme;
    XIM input_method = nullptr;
    XIC input_context = nullptr;
    std::string clipboard_text;
    Clock::time_point save_due = Clock::time_point::max();
    std::unique_ptr<GpuRenderer> renderer;
    Window window = 0;
    Cursor normal_cursor = 0;
    Cursor text_cursor = 0;
    Cursor active_cursor = 0;
    Cursor drag_cursor = 0;
    cairo_surface_t* surface = nullptr;
    cairo_t* cr = nullptr;
    PangoLayout* base_layout = nullptr;
    int font_height = 0;
    int line_height = 0;
    int header_height = 0;
    const std::array<unsigned, 12> colors = {0x24292f, 0x0033b3, 0x6f42c1, 0xa31515,
                                             0x39703a, 0x006b6b, 0x8546a3, 0x795e26,
                                             0x24292f, 0x875a00, 0x005cc5, 0x6f42c1};
    std::vector<SourcePanel> panels;
    std::vector<Vec2> file_sizes;
    std::map<std::string, RsvgHandle*> icons;
    std::map<std::string, GpuImage> gpu_icons;
    std::vector<lsp::Document> documents;
    std::unordered_set<std::string> opened_documents;
    std::vector<Dependency> dependencies;
    GraphLayout graph;
    Vec2 content_size;
    std::unique_ptr<lsp::LanguageServer> language_server;
    std::vector<std::string> token_legend;
    unsigned deprecated_modifier = 0;
    unsigned definition_modifier = 0;
    bool language_ready = false;
    std::string language_failure;
    int width = 800;
    int height = 600;
    Camera camera;
    ZoomAnimation zoom_animation;
    bool initial_view = true;
    unsigned drag_button = 0;
    int dragged_panel = -1;
    bool show_connections = true;
    bool connections_dirty = true;
    GpuMesh connection_mesh;
    std::array<GpuMesh, 2> fold_toggle_meshes;
    std::vector<CanvasRect> header_hits;
    bool drag_moved = false;
    Vec2 last_pointer;
    Vec2 mouse_position;
    bool redraw = true;
    Clock::time_point hover_due = Clock::time_point::max();
    Clock::time_point popup_dismiss_due = Clock::time_point::max();
    int hover_request = 0;
    int navigation_request = 0;
    int list_request = 0;
    std::string hover_prefix;
    std::string popup_text;
    Vec2 popup_origin;
    Vec2 hover_origin;
    CanvasRect popup_bounds{};
    double popup_scroll = 0;
    std::vector<lsp::Json> popup_locations;
    std::vector<JumpRow> jump_rows;
    std::vector<SymbolNode*> popup_symbol_rows;
    std::vector<int> popup_row_depths;
    int list_offset = 0;
    int outline_panel = -1;
    std::string list_title;
    struct SavedView {
        Camera camera;
        int panel = -1;
        Vec2 relative_center;
    };
    std::vector<SavedView> navigation_history;
    std::optional<SourceHit> focused_line;
    struct MathArtwork {
        GpuImage image;
        Vec2 size;
        bool ready = false;
    };
    std::unique_ptr<MathTypesetter> math_typesetter;
    std::map<std::string, MathArtwork> math_artwork;
    unsigned document_generation = 0;
    PangoLayout* popup_layout = nullptr;
    GpuMesh popup_mesh;
    std::string cached_popup;
    int cached_popup_width = 0;
    bool popup_prose = false;
    std::vector<PopupSpan> popup_styles;
    std::vector<PopupMath> popup_math;
    std::vector<bool> popup_code_lines;
    std::vector<bool> cached_popup_code_lines;
    std::vector<std::pair<std::size_t, std::size_t>> popup_inline_code;
    Atom close_message = 0;
    Atom protocols = 0;
    bool running = true;
    bool window_destroyed = false;
    bool auto_build = false, build_running = false, reload_requested = false,
         deferred_reload = false;
    std::unique_ptr<LiveBuild> live_build;
    uint64_t source_generation = 0;
    std::string build_status;
    std::filesystem::file_time_type loaded_binary, observed_binary;
    Clock::time_point rebuild_due = Clock::time_point::max();
    Clock::time_point binary_check_due = Clock::time_point::max();

    bool initialize();
    void initialize_workspace();
    void initialize_reload();
    void queue_rebuild();
    void poll_reload(Clock::time_point now);
    void handle_build_result(const BuildResult& result);
    void request_reload();
    void initialize_intent();
    void intent_request(const std::string& method, lsp::Json params = lsp::Json::object());
    void poll_intent();
    void receive_intent(const lsp::Json& message);
    void capture_intents();
    void attribute_chat_entry(std::size_t index);
    void focus_intent();
    void inspect_intent(Vec2 pointer);
    void draw_intent();
    void draw_intent_controls();
    bool intent_button(const XEvent& event, Vec2 pointer);
    bool intent_key(const XEvent& event);
    void intent_action(const std::string& action);
    void open_intent_conversation(const std::string& id);
    void initialize_chat();
    void poll_agent();
    void flush_agent();
    void focus_chat();
    void draw_conversation();
    double chat_entry_height(std::size_t index);
    std::string chat_entry_source(std::size_t index);
    void prepare_chat_messages();
    void restore_chat_scroll(bool finish);
    void draw_chat_messages();
    void draw_chat_chrome();
    void draw_chat_controls();
    void draw_chat_scrollbar();
    void begin_chat_scroll(Vec2 world);
    void drag_chat_scroll(Vec2 world);
    void draw_input_selection(Vec2 origin);
    void move_chat_caret(int direction, bool extend);
    void draw_chat_questions();
    void respond_chat_question(const std::string& text);
    bool chat_button(const XEvent& event, Vec2 pointer);
    void chat_action(const ChatHit& hit, Vec2 world);
    bool chat_key(const XEvent& event);
    bool chat_shortcut(KeySym key, unsigned state, const XEvent& event);
    void chat_selection(const XEvent& event);
    bool chat_over_text(Vec2 pointer);
    bool open_chat_link(const DocumentView& view, Vec2 point);
    void load_session();
    void save_session();
    void schedule_save();
    int benchmark_run();
    PangoLayout* make_line(const std::string& line, const std::vector<Token>& tokens);
    SourcePanel make_panel(const std::string& path, const std::string& text);
    void update_mesh(SourcePanel& panel);
    void relayout();
    void load_project();
    void poll_project();
    void release_panel(SourcePanel& panel);
    void replace_source(int index, const std::string& source);
    void apply_project_change(const ProjectChange& change);
    void refresh_project_geometry();
    void apply_styles(SourcePanel& panel);
    Viewport viewport();
    void hide_popup();
    void show_popup(std::string text, Vec2 origin, bool syntax = false);
    SourceHit hit_test(Vec2 screen);
    int panel_at(Vec2 screen);
    void refresh_list();
    void show_outline(int index);
    bool definition_at_pointer();
    int query_at_pointer(const std::string& method);
    bool jump_to(const lsp::Json& location);
    void toggle_fold(int index, int source);
    void fit_width();
    void draw();

    void handle_button(const XEvent& event);
    void zoom_wheel(unsigned button, Vec2 pointer);
    void handle_motion(const XEvent& event);
    void update_cursor();
    bool over_text(Vec2 screen);
    void handle_release(const XEvent& event);
    void handle_key(const XEvent& event);
    void handle_message(const lsp::Message& message);
    void handle_event(const XEvent& event);
    void remember_view();
    bool go_back();

    void update_timers(Clock::time_point now);
    void wait_for_work(Clock::time_point next_frame);
    void draw_connections(const Viewport& view);
    void draw_files(const Viewport& view);
    void draw_headers(const Viewport& view);
    void draw_fold_toggle();
    CanvasRect fold_toggle_bounds(const SourcePanel& panel, int line);
    SourceHit fold_at(Vec2 screen);
    void draw_popup();
    GpuPath dependency_path(std::size_t index);
    Vec2 dependency_anchor(const SourcePanel& panel, int line, std::size_t byte, bool outgoing);
    void rebuild_connections();
    void update_popup_mesh(int layout_width);
    void poll_math();
    void shape_document_math(PangoAttrList* attributes, const std::vector<PopupMath>& math,
                             int layout_width);
    void upload_document_mesh(GpuMesh& mesh, PangoLayout* layout,
                              const std::vector<PopupMath>& math);
    MathArtwork load_math_artwork(const std::string& svg);
    void style_document_spans(PangoAttrList* attributes, const std::vector<PopupSpan>& spans);
    void configure_document(PangoLayout* layout, const PopupDocument& document, int width,
                            bool prose);
    void update_document(DocumentView& view, const std::string& source, int width,
                         bool prose = true);
    void add_popup_syntax(PangoAttrList* attributes, const std::string& text, std::size_t offset,
                          HighlightState& state);
    void load_icon(const std::string& icon_name);
    void discover_braces(SourcePanel& panel, const std::string& line,
                         const std::vector<Token>& tokens, std::vector<int>& brace_stack);
    void apply_diagnostics(const SourcePanel& panel, std::size_t line, PangoAttrList* attributes);
    void language_initialized(const lsp::Message& message);
    void language_structure(const lsp::Message& message);
    void language_styles(const lsp::Message& message);
    void language_hover(const lsp::Message& message);
    void language_definition(const lsp::Message& message);
    void language_references(const lsp::Message& message);
    void language_completion(const lsp::Message& message);
    bool popup_button(const XEvent& event, Vec2 pointer, bool in_popup);
    void begin_drag(const XEvent& event, Vec2 pointer, const Viewport& view);
};
