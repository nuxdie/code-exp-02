#pragma once
#include "agent_activity.h"
#include "agent_client.h"
#include "canvas_selection.h"
#include "canvas_window.h"
#include "chat_canvas.h"
#include "connection_hud.h"
#include "conversation.h"
#include "directory_picker.h"
#include "document_view.h"
#include "intent_margin.h"
#include "overview.h"
#include "popup_markup.h"
#include "project_watch.h"
#include "search_canvas.h"
#include "source_graph.h"
#include "source_panel.h"
#include "syntax.h"
#include "system_theme.h"
#include "text.h"
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
#include <format>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <pango/pangocairo.h>
#include <poll.h>
#include <set>
#include <span>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

class Application {
    friend struct ApplicationTestAccess;

  public:
    static constexpr int reload_exit_code = 75;
    explicit Application(std::string directory = "") : launch_directory(std::move(directory)) {}
    const std::string& directory() const { return launch_directory; }
    int run();
    ~Application();

  private:
    using Clock = std::chrono::steady_clock;
    bool benchmark = false;
    Display* display = nullptr;
    std::filesystem::path executable;
    std::string launch_directory;
    std::unique_ptr<DirectoryPicker> directory_picker;
    std::string workspace_root;
    std::string workspace_build;
    std::unique_ptr<ProjectWatcher> project_watcher;
    struct ImagePreview {
        SurfaceHandle pixels;
        std::string error;
    };
    struct WorkspaceLayout {
        std::vector<Dependency> dependencies;
        std::unique_ptr<ProjectWatcher> watcher;
    };
    struct WorkspaceFile {
        std::optional<std::string> source;
        std::optional<ImagePreview> image;
    };
    struct WorkspaceLoad {
        std::future<std::vector<ProjectFile>> reading;
        std::future<WorkspaceFile> detail;
        ProjectSnapshot snapshot;
        std::vector<GraphFile> files;
        FileIndex index;
        std::vector<Dependency> edges;
        // Where every file will be, decided before any is read and only adjusted in place.
        std::vector<CanvasRect> plan;
        lsp::Json session; // the saved canvas, when its positions can be trusted
        std::vector<std::size_t> panel_indices;
        std::size_t next = 0;
        Clock::time_point draw_due{};
        std::future<WorkspaceLayout> layout;
    };
    std::optional<WorkspaceLoad> workspace_load;
    int screen = 0;
    std::unique_ptr<ChatCanvas> chat;
    std::vector<std::unique_ptr<ChatCanvas>> chat_windows;
    IntentMargin intent_margin;
    std::vector<std::future<void>> intent_writes;
    std::unique_ptr<SystemTheme> system_theme;
    XIM input_method = nullptr;
    XIC input_context = nullptr;
    std::string clipboard_text, primary_text;
    CanvasSelection text_selection;
    SearchCanvas search;
    bool shortcuts_visible = false;
    static constexpr const char* monospace_family = "Victor Mono";
    void configure_source_font();
    Clock::time_point save_due = Clock::time_point::max();
    std::unique_ptr<GpuRenderer> renderer;
    Window window = 0;
    Cursor normal_cursor = 0;
    Cursor text_cursor = 0;
    Cursor link_cursor = 0;
    Cursor active_cursor = 0;
    Cursor drag_cursor = 0;
    SurfaceHandle surface;
    CairoHandle cr;
    LayoutHandle base_layout;
    int font_height = 0;
    int line_height = 0;
    int header_height = 0;
    const std::array<unsigned, 12> colors = {0x24292f, 0x0033b3, 0x6f42c1, 0xa31515,
                                             0x39703a, 0x006b6b, 0x8546a3, 0x795e26,
                                             0x24292f, 0x875a00, 0x005cc5, 0x6f42c1};
    std::vector<SourcePanel> panels;
    std::vector<Vec2> file_sizes;
    std::vector<lsp::Document> documents;
    std::unordered_set<std::string> opened_documents;
    std::vector<Dependency> dependencies;
    GraphLayout graph;
    Vec2 content_size;
    std::unique_ptr<lsp::LanguageServer> language_server;
    std::vector<std::string> token_legend;
    unsigned deprecated_modifier = 0;
    unsigned definition_modifier = 0;
    unsigned declaration_modifiers = 0;
    bool language_ready = false;
    std::string language_failure;
    int width = 800;
    int height = 600;
    Camera camera;
    ZoomAnimation zoom_animation;
    bool initial_view = true;
    unsigned drag_button = 0;
    int dragged_panel = -1;
    CanvasWindowManager window_manager;
    std::vector<CanvasRect> window_headers;
    enum class WindowKind { Canvas, Source, Chat, Popup, Search };
    WindowKind window_at(Vec2 pointer);
    bool begin_window_drag(Vec2& origin, Vec2 pointer);
    void draw_window_title(DocumentView& title, const std::string& text, CanvasRect world,
                           CanvasRect& hit, std::vector<CanvasRect>& occupied);
    bool connections_dirty = true;
    ConnectionSubject connection_subject;
    std::pair<int, int> connection_requests{0, 0}; // definition, references
    std::vector<ConnectionLink> connection_links;
    std::vector<ConnectionMarker> connection_markers;
    std::map<std::string, DocumentView> connection_labels;
    Camera connection_camera;
    Vec2 connection_size;
    int connection_pointed = -1; // the marker under the pointer, whose arrow stands out
    struct ConnectionWord {
        CanvasRect bounds; // on screen
        std::array<float, 4> color;
    };
    std::vector<ConnectionWord> connection_words;
    CanvasRect connection_subject_box{}; // on screen, empty while out of view
    struct AgentMarker {
        CanvasRect bounds;
        std::string uri;
        int line = 0;
    };
    std::vector<AgentMarker> agent_markers;
    std::map<std::string, DocumentView> agent_marker_labels;
    void draw_agent_markers();
    void show_connections_at(Vec2 pointer);
    bool clear_connections();
    void add_connection_link(ConnectionLink link);
    void text_connections();
    bool receive_connections(const lsp::Message& message);
    CanvasRect word_bounds(const SourcePanel& panel, int line, std::size_t begin,
                           std::size_t end) const;
    CanvasRect connection_box(const ConnectionEnd& end);
    void connect_boxes(std::vector<GpuPath>& paths, const CanvasRect& from, const CanvasRect& to,
                       std::array<float, 4> color, float width);
    void mark_connection(const CanvasRect& view, std::vector<std::size_t> members,
                         const ConnectionEnd& end, std::vector<MarkerPart> parts);
    std::vector<std::size_t> place_connection_ends(const CanvasRect& view);
    void connection_arrows(std::vector<GpuPath>& paths, const CanvasRect& subject,
                           const std::vector<std::size_t>& visible, bool to_markers);
    void layout_connections();
    void draw_connection_markers();
    const ConnectionMarker* connection_marker_at(Vec2 pointer);
    bool connection_marker_button(const XEvent& event, Vec2 pointer);
    bool connection_key(const XEvent& event);
    GpuMesh connection_mesh;
    std::array<GpuMesh, 2> fold_toggle_meshes;
    GpuMesh chat_icon_mesh;
    std::vector<CanvasRect> header_hits;
    std::vector<FolderRegion> regions;
    std::map<std::string, DocumentView> overview_labels;
    bool drag_moved = false;
    Vec2 last_pointer;
    Vec2 mouse_position;
    bool redraw = true;
    Clock::time_point chat_animation_due = Clock::time_point::max();
    AgentActivity agent_activity;
    std::map<std::string, DocumentView> activity_labels;
    std::map<std::string, std::pair<CanvasRect, Clock::time_point>> activity_deleted;
    Clock::time_point activity_animation_due = Clock::time_point::max();
    GpuMesh activity_mesh;
    Clock::time_point hover_due = Clock::time_point::max();
    Clock::time_point popup_dismiss_due = Clock::time_point::max();
    int hover_request = 0;
    int navigation_request = 0;
    std::string hover_prefix;
    std::string popup_text;
    Vec2 popup_origin;
    Vec2 hover_origin;
    CanvasRect popup_bounds{};
    double popup_scroll = 0;
    struct SavedView {
        Camera camera;
        std::string path;
        Vec2 relative_center;
    };
    std::vector<SavedView> navigation_history;
    std::vector<SavedView> navigation_forward;
    std::optional<SavedView> pending_view;
    Clock::time_point navigation_input;
    bool navigation_continuous = false;
    std::optional<SourceHit> focused_line;
    unsigned document_generation = 0;
    LayoutHandle popup_layout;
    GpuMesh popup_mesh;
    std::string cached_popup;
    int cached_popup_width = 0;
    bool popup_prose = false;
    std::vector<PopupSpan> popup_styles;
    std::vector<bool> popup_code_lines;
    std::vector<bool> cached_popup_code_lines;
    std::vector<std::pair<std::size_t, std::size_t>> popup_inline_code;
    Atom close_message = 0;
    Atom protocols = 0;
    bool running = true;
    bool window_destroyed = false;
    bool reload_requested = false; // re-launch on another directory

    bool initialize();
    bool initialize_window();
    int run_window();
    void receive_agent_activity(const lsp::Json& message);
    std::optional<CanvasRect> activity_bounds(const ActivityTouch& touch);
    void advance_agent_activity();
    void draw_activity_highlights();
    void draw_agent_activity();
    void begin_workspace_load();
    void advance_workspace_load();
    bool add_workspace_panel();
    void start_workspace_file();
    void prepare_workspace_preview(std::vector<ProjectFile> files);
    void draw_workspace_preview(const Viewport& view);
    void finish_workspace_load();
    void restore_file_layouts(const lsp::Json& files, bool placed);
    lsp::Json read_session() const;
    bool restore_folds(SourcePanel& panel, const lsp::Json& saved);
    void restore_window(const lsp::Json& state);
    Vec2 estimated_size(const ProjectFile& file, double cell) const;
    void settle_plan(std::size_t index, Vec2 size);
    void finish_plan();
    void load_agent_windows(const lsp::Json& state, const char* environment_thread);
    void open_directory_picker();
    void poll_directory_picker();
    lsp::LanguageServer* server_for(const SourcePanel& panel);
    bool open_document_link(Vec2 pointer);
    std::string text_link_at(Vec2 pointer);
    bool open_link(const std::string& destination);
    void language_links(const lsp::Message& message);
    void load_intent_margin();
    std::string record_turn_intent(const lsp::Json& call);
    void poll_intent_margin();
    std::optional<SourceHit> source_line_at(Vec2 screen);
    CanvasRect intent_run_bounds(const SourcePanel& panel, const IntentRun& run);
    void draw_intent_run(const SourcePanel& panel, const IntentRun& run);
    void draw_intent_margin(const Viewport& view);
    void draw_intent_note();
    void select_intent_at(Vec2 pointer);
    bool clear_intent_selection();
    bool intent_margin_button(const XEvent& event, Vec2 pointer);
    bool intent_margin_key(const XEvent& event);
    void draw_intent();
    void draw_intent_controls();
    bool intent_button(const XEvent& event, Vec2 pointer);
    bool begin_intent_drag(Vec2 pointer, Vec2 world);
    void activate_intent_at(Vec2 pointer, Vec2 world);
    bool intent_key(const XEvent& event);
    void intent_action(const std::string& action);
    void open_intent_conversation(const std::string& id);
    void draw_chat_card();
    void open_search();
    void close_search();
    void update_search();
    void draw_search();
    void draw_search_match(SearchMatch& match, Vec2 origin, double row_width);
    void search_line(DocumentView& view, const std::string& text, int row_width, bool code,
                     bool cmake = false);
    void move_search(int direction);
    void open_search_match(std::size_t index);
    bool search_key(const XEvent& event);
    bool search_edit_key(const XEvent& event, KeySym key);
    bool search_button(const XEvent& event, Vec2 pointer);
    void paste_search(const XEvent& event);
    std::optional<std::string> pasted_text(const XEvent& event);
    bool canvas_shortcut_key(const XEvent& event);
    void show_shortcuts();
    bool shortcuts_key(KeySym key);
    bool handle_canvas_tool(const lsp::Json& message);
    void poll_agent_window();
    void start_agent_windows();
    ChatCanvas* chat_at(Vec2 pointer);
    bool activate_chat(Vec2 pointer);
    void spawn_agent_window(Vec2 world, lsp::Json context);
    void close_agent_window();
    static bool kept_chat(const ChatCanvas& card);
    void start_agent(ChatCanvas& card);
    bool reopen_chat(Vec2 pointer);
    std::vector<CanvasRect> conversation_bounds() const;
    void shelve_conversations();
    void dismiss_empty_chat(const XEvent& event, Vec2 pointer);
    bool close_window_button(const XEvent& event, Vec2 pointer);
    bool agent_window_key(const XEvent& event);
    void open_chat_input(Vec2 pointer);
    lsp::Json agent_context(Vec2 pointer);
    void refresh_agent_context();
    void render_agent_context();
    void prepare_chat_prompt();
    std::optional<std::pair<std::size_t, std::size_t>>
    prompt_source_range(const SourcePanel& panel) const;
    bool agents_pending();
    Clock::time_point agent_reconnect_due();
    lsp::Json save_agent_window();
    void load_agent_window(ChatCanvas& card, const lsp::Json& state);
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
    void draw_chat_working(double y);
    void draw_chat_chrome();
    void draw_chat_header(std::vector<CanvasRect>& occupied);
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
    void chat_message_action(const ChatHit& hit, Vec2 world);
    bool chat_key(const XEvent& event);
    bool chat_shortcut(KeySym key, unsigned state, const XEvent& event);
    void chat_selection(const XEvent& event);
    bool chat_over_text(Vec2 pointer);
    bool open_chat_link(const DocumentView& view, Vec2 point);
    TextTarget text_at(Vec2 pointer);
    TextTarget active_text_target();
    TextTarget chat_text_target(Vec2 pointer);
    TextTarget source_text_target(Vec2 pointer);
    std::size_t text_byte_at(const TextTarget& target, Vec2 pointer, bool* inside = nullptr);
    std::size_t source_byte_at(const SourcePanel& panel, Vec2 pointer);
    TextInput& selected_input();
    bool begin_text_selection(const XEvent& event, Vec2 pointer);
    void move_text_selection(Vec2 pointer);
    void end_text_selection(const XEvent& event);
    void selection_click(const XEvent& event);
    bool selection_key(const XEvent& event);
    std::string selected_text();
    void draw_selection_layout(PangoLayout* layout, Vec2 origin, double scale,
                               std::pair<std::size_t, std::size_t> range,
                               std::array<float, 4> color = {0.70f, 0.81f, 0.98f, 0.85f});
    void draw_source_selection(const SourcePanel& panel, Vec2 origin);
    void draw_source_range(const SourcePanel& panel, Vec2 origin,
                           std::pair<std::size_t, std::size_t> range, bool context);
    void load_session();
    void save_session();
    void schedule_save();
    int benchmark_run();
    LayoutHandle make_line(std::string_view line, const std::vector<Token>& tokens,
                           const ProseLine* prose = nullptr);
    SourcePanel make_panel(const std::string& path, const std::string& text,
                           ImagePreview* preview = nullptr);
    void update_mesh(SourcePanel& panel);
    static ImagePreview decode_image(const std::string& bytes);
    void load_image_panel(SourcePanel& panel, ImagePreview preview, int title_width);
    void update_image_geometry(SourcePanel& panel, int title_width);
    void relayout();
    void resize_in_place(const SourcePanel& panel, double old_height);
    void place_in_folder(SourcePanel& panel, const std::set<std::string>& unplaced = {});
    void apply_graph_layout();
    void poll_project();
    void replace_source(int index, const std::string& source);
    void apply_project_change(const ProjectChange& change);
    void refresh_project_geometry(bool settle = true);
    void apply_styles(SourcePanel& panel);
    Viewport viewport() const;
    void hide_popup();
    void show_popup(const std::string& text, Vec2 origin, bool syntax = false);
    SourceHit hit_test(Vec2 screen);
    int panel_at(Vec2 screen);
    int query_at_pointer(const std::string& method);
    bool jump_to(const lsp::Json& location);
    void toggle_fold(int index, int source);
    void draw();

    void handle_button(const XEvent& event);
    void handle_canvas_button(const XEvent& event, Vec2 pointer, const Viewport& view);
    void zoom_wheel(unsigned button, Vec2 pointer);
    void handle_motion(const XEvent& event);
    void handle_drag_motion(Vec2 pointer);
    void update_pointer_hover(Vec2 pointer);
    void update_cursor();
    bool over_text(Vec2 screen);
    void handle_release(const XEvent& event);
    void handle_key(const XEvent& event);
    bool cancel_pointer_drag(const XEvent& event);
    void handle_message(const lsp::Message& message);
    void handle_event(const XEvent& event);
    SavedView current_view();
    Camera saved_camera(const SavedView& saved);
    void remember_view(bool continuous = false);
    void finish_view();
    bool navigate_view(bool forward);
    bool navigation_key(const XEvent& event);
    bool navigation_button(unsigned button);
    bool camera_key(KeySym key);
    bool go_back();
    bool go_forward();
    lsp::Json save_navigation();
    void load_navigation(const lsp::Json& state);

    void update_timers(Clock::time_point now);
    void wait_for_work(Clock::time_point next_frame);
    void draw_connections(const Viewport& view);
    void draw_files(const Viewport& view);
    void draw_headers(const Viewport& view);
    void draw_regions(const Viewport& view);
    void draw_region_names(const Viewport& view, std::vector<CanvasRect>& taken);
    void draw_outline(SourcePanel& panel, const Viewport& view, std::vector<CanvasRect>& taken);
    DocumentView& overview_label(const std::string& text);
    void draw_fold_toggle();
    CanvasRect fold_toggle_bounds(const SourcePanel& panel, int line) const;
    SourceHit fold_at(Vec2 screen);
    void draw_popup();
    void style_shortcuts(int layout_width);
    void draw_shortcut_keys(Vec2 origin);
    void refresh_symbol_sources(const std::vector<ProjectChange>& changes);
    void rebuild_connections();
    void update_popup_mesh(int layout_width);
    void style_document_spans(PangoAttrList* attributes, const std::vector<PopupSpan>& spans);
    void configure_document(PangoLayout* layout, const PopupDocument& document, int width,
                            bool prose);
    void update_document(DocumentView& view, const std::string& source, int width,
                         bool prose = true);
    void add_popup_syntax(PangoAttrList* attributes, std::string_view text, std::size_t offset,
                          HighlightState& state);
    void apply_diagnostics(const SourcePanel& panel, std::size_t line, PangoAttrList* attributes);
    void language_error(const lsp::Message& message);
    void language_initialized(const lsp::Message& message);
    void language_structure(const lsp::Message& message);
    void language_styles(const lsp::Message& message);
    void language_hover(const lsp::Message& message);
    void language_definition(const lsp::Message& message);
    bool popup_button(const XEvent& event, bool in_popup);
    void begin_drag(const XEvent& event, Vec2 pointer, const Viewport& view, bool pan_only = false);
};
