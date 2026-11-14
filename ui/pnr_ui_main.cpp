// PNR Desktop UI (slice UI-1), GPL-2.0: SDL2 window + Dear ImGui (MIT).
// A real native window that drives the REAL library, scan, import and launch
// code through ui_model. Nothing here is mock data.
// Status: Implemented. Tested on Xvfb (a virtual X display) with SDL's
// renderer; UNTESTED on a real display or GPU, untested on Windows (Blocked).
#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"
#include "launcher/launcher_core.hpp"
#include "ui/ui_model.hpp"

namespace {

struct Opts {
  std::string db;
  int w = 1366, h = 768;
  int frames = -1;                                  // quit after N frames (tests, recordings)
  std::vector<std::pair<int, std::string>> shots;   // save a BMP of the window at frame N
  std::string select_id, auto_launch, scan_dir;     // test hooks: same code paths as the mouse
  bool auto_sandbox = false;
};

const ImVec4 kAccent(0.36f, 0.55f, 1.00f, 1.0f);
const ImVec4 kMuted(0.60f, 0.63f, 0.70f, 1.0f);

ImVec4 compat_color(const std::string& s) {
  if (s == "Verified Playable") return ImVec4(0.30f, 0.85f, 0.45f, 1);
  if (s == "Playable With Issues") return ImVec4(0.95f, 0.75f, 0.25f, 1);
  if (s == "Launches Only") return ImVec4(0.40f, 0.65f, 1.00f, 1);
  if (s == "Unsupported") return ImVec4(0.95f, 0.40f, 0.40f, 1);
  if (s == "Blocked by Missing Dependency") return ImVec4(0.95f, 0.55f, 0.25f, 1);
  return ImVec4(0.65f, 0.67f, 0.72f, 1);  // Untested
}

void apply_style() {
  ImGui::StyleColorsDark();
  ImGuiStyle& st = ImGui::GetStyle();
  st.WindowRounding = 0; st.ChildRounding = 8; st.FrameRounding = 6; st.GrabRounding = 6; st.PopupRounding = 6;
  st.FramePadding = ImVec2(10, 7); st.ItemSpacing = ImVec2(10, 9); st.CellPadding = ImVec2(10, 8);
  st.WindowPadding = ImVec2(16, 16); st.ScrollbarRounding = 8;
  ImVec4* c = st.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.08f, 0.10f, 1);
  c[ImGuiCol_ChildBg] = ImVec4(0.105f, 0.115f, 0.145f, 1);
  c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.165f, 0.205f, 1);
  c[ImGuiCol_Button] = ImVec4(0.20f, 0.30f, 0.62f, 1);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.27f, 0.40f, 0.78f, 1);
  c[ImGuiCol_ButtonActive] = ImVec4(0.22f, 0.34f, 0.70f, 1);
  c[ImGuiCol_Header] = ImVec4(0.20f, 0.30f, 0.62f, 0.55f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.27f, 0.40f, 0.78f, 0.60f);
  c[ImGuiCol_HeaderActive] = ImVec4(0.27f, 0.40f, 0.78f, 0.80f);
  c[ImGuiCol_TableHeaderBg] = ImVec4(0.13f, 0.145f, 0.18f, 1);
  c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
  c[ImGuiCol_CheckMark] = kAccent;
}

const char* find_font(const std::vector<const char*>& paths) {
  for (const char* p : paths) if (std::filesystem::exists(p)) return p;
  return nullptr;
}

struct App {
  pnr::ui::UiModel& model;
  const Opts& opts;
  App(pnr::ui::UiModel& m, const Opts& o) : model(m), opts(o) {}
  char search[128] = "";
  std::string selected;
  bool sandbox = false;
  char scan_path[512] = "";
  bool scanned = false;
  std::string scan_msg;
  pnr::launcher::ScanReport scan_report;
  std::string import_msg;
  bool import_ok = false;
  ImFont* mono = nullptr;
  std::string renderer_name, video_driver;
  bool show_scan = true;

  void do_launch(const std::string& id) {
    std::string why;
    if (!model.start_launch(id, sandbox, &why)) import_msg = "Cannot launch: " + why;
  }
  void do_scan() {
    scan_report = {};
    auto st = model.scan(scan_path, &scan_report);
    scanned = st.ok;
    scan_msg = st ? "" : st.error.message;
    import_msg.clear();
  }
  void do_import(const std::string& dir, const std::string& file) {
    auto o = model.import_package((std::filesystem::path(dir) / file).string());
    import_ok = o.ok;
    import_msg = o.ok ? "Imported " + o.title + " " + o.version + ". New games start as Untested." : "Import failed: " + o.error;
    do_scan();
  }

  void sidebar(float h) {
    ImGui::BeginChild("side", ImVec2(236, h), false, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushFont(nullptr, 34.0f);
    ImGui::TextColored(kAccent, "PNR");
    ImGui::PopFont();
    ImGui::TextColored(kMuted, "PlayStation Native Runtime");
    ImGui::Dummy(ImVec2(0, 14));
    ImGui::Selectable("  Library", true, 0, ImVec2(0, 34));
    struct Soon { const char* name; const char* tag; };
    for (Soon s : {Soon{"Game details", "UI-2"}, Soon{"Store", "UI-3"}, Soon{"Settings", "UI-4"}, Soon{"Hardware", "planned"}, Soon{"Diagnostics", "planned"}, Soon{"Reports", "planned"}}) {
      ImGui::BeginDisabled();
      ImGui::Selectable((std::string("  ") + s.name).c_str(), false, 0, ImVec2(0, 34));
      ImGui::EndDisabled();
      ImGui::SameLine(ImGui::GetWindowWidth() - 84);
      ImGui::TextColored(ImVec4(0.45f, 0.47f, 0.55f, 1), "%s", s.tag);
    }
    ImGui::SetCursorPosY(h - 150);
    ImGui::Separator();
    ImGui::TextColored(kMuted, "Linux x86-64 only.");
    ImGui::TextColored(kMuted, "Windows: Blocked.");
    ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 12);
    ImGui::TextColored(kMuted, "Untested on a real display or GPU.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
  }

  void table(float w, float h) {
    ImGui::BeginChild("tbl", ImVec2(w, h), true);
    auto idx = model.filter(search);
    if (model.games().empty()) {
      ImGui::Dummy(ImVec2(0, 40));
      ImGui::PushFont(nullptr, 24.0f);
      ImGui::TextColored(kMuted, "Your library is empty");
      ImGui::PopFont();
      ImGui::TextWrapped("Add games from a folder with the panel below. PNR only runs packages you point it at; there is no default source.");
    } else if (idx.empty()) {
      ImGui::Dummy(ImVec2(0, 40));
      ImGui::TextColored(kMuted, "No game matches \"%s\".", search);
    } else if (ImGui::BeginTable("games", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch, 3.0f);
      ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 2.2f);
      ImGui::TableSetupColumn("Runnable", ImGuiTableColumnFlags_WidthStretch, 1.1f);
      ImGui::TableSetupColumn("Launches", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableHeadersRow();
      for (size_t i : idx) {
        const auto& g = model.games()[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        bool sel = (g.game_id == selected);
        if (ImGui::Selectable((g.title + "##" + g.game_id).c_str(), sel, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, 24))) selected = g.game_id;
        ImGui::TableNextColumn(); ImGui::TextColored(kMuted, "%s", g.version.c_str());
        ImGui::TableNextColumn(); ImGui::TextColored(compat_color(g.compatibility), "\xE2\x97\x8F %s", g.compatibility.c_str());
        ImGui::TableNextColumn();
        if (g.runnable) ImGui::Text("yes"); else ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "no");
        ImGui::TableNextColumn(); ImGui::Text("%lld", static_cast<long long>(g.launch_count));
      }
      ImGui::EndTable();
    }
    ImGui::EndChild();
  }

  void detail(float w, float h) {
    ImGui::BeginChild("detail", ImVec2(w, h), true);
    pnr::launcher::GameEntry g;
    bool have = false;
    for (const auto& e : model.games()) if (e.game_id == selected) { g = e; have = true; }
    if (!have) {
      ImGui::Dummy(ImVec2(0, 30));
      ImGui::TextColored(kMuted, "Select a game to see details and launch it.");
      ImGui::EndChild();
      return;
    }
    ImGui::PushFont(nullptr, 28.0f);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(g.title.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::TextColored(kMuted, "%s  v%s", g.game_id.c_str(), g.version.c_str());
    ImGui::Spacing();
    ImGui::TextColored(compat_color(g.compatibility), "\xE2\x97\x8F %s", g.compatibility.c_str());
    if (!g.compatibility_notes.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(kMuted, "Note: %s", g.compatibility_notes.c_str()); ImGui::PopTextWrapPos(); }
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(ImVec4(0.5f, 0.52f, 0.6f, 1), g.compat_human_set ? "Set by a person." : "Set automatically. PNR never marks a game Playable on its own.");
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    ImGui::TextColored(kMuted, "Installed at");
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(g.install_path.empty() ? "(not installed)" : g.install_path.c_str());
    ImGui::PopTextWrapPos();
    ImGui::TextColored(kMuted, "Integrity: %s   Launches: %lld", g.integrity.empty() ? "unknown" : g.integrity.c_str(), static_cast<long long>(g.launch_count));
    ImGui::Separator();
    std::string blocker = model.launch_blocker(g);
    ImGui::Checkbox("Run in sandbox (partial isolation)", &sandbox);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Linux namespaces, seccomp denylist and a private root.\nPARTIAL: not a complete sandbox. Refuses to start if unusable.");
    if (!blocker.empty()) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.55f, 0.32f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.66f, 0.38f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.48f, 0.28f, 1));
    if (ImGui::Button(model.launching() ? "Running..." : "Launch", ImVec2(-1, 48))) do_launch(g.game_id);
    ImGui::PopStyleColor(3);
    if (!blocker.empty()) ImGui::EndDisabled();
    if (!blocker.empty()) ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "Cannot launch: %s", blocker.c_str());
    auto r = model.last_launch();
    if (r.game_id == g.game_id && (r.running || r.finished)) {
      ImGui::Spacing();
      ImVec4 col = r.running ? kAccent : (r.clean ? ImVec4(0.35f, 0.85f, 0.5f, 1) : ImVec4(0.95f, 0.45f, 0.4f, 1));
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(col, "%s%s", r.summary.c_str(), r.sandbox ? "  [sandbox requested]" : "");
      ImGui::PopTextWrapPos();
      if (!r.output.empty()) {
        ImGui::TextColored(kMuted, "Game output");
        ImGui::BeginChild("out", ImVec2(0, 0), true);
        if (mono) ImGui::PushFont(mono, 16.0f);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(r.output.c_str());
        ImGui::PopTextWrapPos();
        if (mono) ImGui::PopFont();
        ImGui::EndChild();
      }
    }
    ImGui::EndChild();
  }

  void add_games(float w, float h) {
    ImGui::BeginChild("add", ImVec2(w, h), true);
    ImGui::TextColored(kAccent, "Add games from a folder");
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "  (read-only scan; nothing runs until you press Launch)");
    ImGui::SetNextItemWidth(w - 220);
    bool enter = ImGui::InputTextWithHint("##scanpath", "Folder with .pnrgame packages, for example /home/you/games", scan_path, sizeof scan_path, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Scan folder", ImVec2(150, 0)) || enter) do_scan();
    if (!scan_msg.empty()) ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "%s", scan_msg.c_str());
    if (!import_msg.empty()) ImGui::TextColored(import_ok ? ImVec4(0.35f, 0.85f, 0.5f, 1) : ImVec4(0.95f, 0.5f, 0.4f, 1), "%s", import_msg.c_str());
    if (scanned) {
      ImGui::BeginChild("scanres", ImVec2(0, 0), false);
      if (scan_report.items.empty()) ImGui::TextColored(kMuted, "No .pnrgame packages or ELF files found in that folder.");
      for (const auto& it : scan_report.items) {
        if (it.kind == "pnrgame") {
          if (it.ok) {
            ImGui::Text("%s  -  %s %s", it.file.c_str(), it.title.c_str(), it.version.c_str());
            ImGui::SameLine();
            if (it.in_library) ImGui::TextColored(kMuted, "already in library");
            else if (ImGui::Button(("Import##" + it.file).c_str())) do_import(scan_path, it.file);
          } else ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "%s  -  not importable: %s", it.file.c_str(), it.error.c_str());
        } else {
          ImGui::TextColored(kMuted, "%s  -  ELF program (%s). Not importable directly; package it first.", it.file.c_str(), it.ok ? "valid x86-64" : it.error.c_str());
        }
      }
      if (scan_report.skipped) ImGui::TextColored(kMuted, "%d other files skipped.", scan_report.skipped);
      ImGui::EndChild();
    }
    ImGui::EndChild();
  }

  void frame(float W, float H) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::Begin("root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
    const float pad = 16, status_h = 34;
    const float inner_h = H - 2 * pad - status_h;
    sidebar(inner_h);
    ImGui::SameLine();
    ImGui::BeginGroup();
    float mw = W - 236 - 3 * pad - 8;
    ImGui::PushFont(nullptr, 30.0f);
    ImGui::TextUnformatted("Library");
    ImGui::PopFont();
    ImGui::SameLine(0, 24);
    ImGui::SetNextItemWidth(360);
    ImGui::InputTextWithHint("##search", "Search title, id or status", search, sizeof search);
    ImGui::SameLine(mw - 110);
    if (ImGui::Button("Refresh", ImVec2(110, 0))) model.refresh();
    ImGui::TextColored(kMuted, "%zu game%s in this library", model.games().size(), model.games().size() == 1 ? "" : "s");
    float top_h = inner_h - 100;
    float add_h = std::min(220.0f, std::max(150.0f, top_h * 0.30f));
    float body_h = top_h - add_h - ImGui::GetStyle().ItemSpacing.y;
    float dw = std::min(440.0f, std::max(360.0f, mw * 0.36f));
    table(mw - dw - 10, body_h);
    ImGui::SameLine();
    detail(dw, body_h);
    add_games(mw, add_h);
    ImGui::EndGroup();
    ImGui::SetCursorPosY(H - status_h - 6);
    ImGui::TextColored(kMuted, "Library: %s   |   video: %s, renderer: %s   |   Windows: Blocked   |   untested on real display/GPU", model.db_path().c_str(), video_driver.c_str(), renderer_name.c_str());
    ImGui::End();
  }
};

bool save_bmp(SDL_Renderer* ren, const std::string& path) {
  int w = 0, h = 0;
  SDL_GetRendererOutputSize(ren, &w, &h);
  SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
  if (!s) return false;
  bool ok = SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0 && SDL_SaveBMP(s, path.c_str()) == 0;
  SDL_FreeSurface(s);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  Opts o;
  if (const char* e = std::getenv("PNR_LIBRARY_DB")) o.db = e;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--db") o.db = next();
    else if (a == "--size") { std::string v = next(); std::sscanf(v.c_str(), "%dx%d", &o.w, &o.h); }
    else if (a == "--frames") o.frames = std::atoi(next().c_str());
    else if (a == "--shot") { std::string v = next(); auto c = v.find(':'); if (c != std::string::npos) o.shots.emplace_back(std::atoi(v.substr(0, c).c_str()), v.substr(c + 1)); }
    else if (a == "--select") o.select_id = next();
    else if (a == "--auto-launch") o.auto_launch = next();
    else if (a == "--auto-sandbox") o.auto_sandbox = true;
    else if (a == "--scan-dir") o.scan_dir = next();
    else if (a == "--help" || a == "-h") {
      std::puts("pnr-ui [--db LIBRARY.db] [--size WxH]\n"
                "test hooks (same code paths as the mouse): --frames N --shot N:file.bmp --select ID --auto-launch ID [--auto-sandbox] --scan-dir DIR");
      return 0;
    } else { std::fprintf(stderr, "pnr-ui: unknown option %s\n", a.c_str()); return 64; }
  }
  if (o.db.empty()) o.db = "./pnr_data/library.db";

  pnr::ui::UiModel model(o.db);
  std::string err;
  if (!model.open(&err)) { std::fprintf(stderr, "pnr-ui: cannot open the library %s: %s\n", o.db.c_str(), err.c_str()); return 2; }

  if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::fprintf(stderr, "pnr-ui: no usable display (SDL: %s)\n", SDL_GetError()); return 3; }
  SDL_Window* win = SDL_CreateWindow("PNR - PlayStation Native Runtime", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, o.w, o.h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
  if (!win) { std::fprintf(stderr, "pnr-ui: cannot create a window: %s\n", SDL_GetError()); return 3; }
  SDL_SetWindowMinimumSize(win, 1024, 600);
  SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
  if (!ren) { std::fprintf(stderr, "pnr-ui: cannot create a renderer: %s\n", SDL_GetError()); return 3; }
  SDL_RendererInfo ri{};
  SDL_GetRendererInfo(ren, &ri);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  apply_style();
  const char* sans = find_font({"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf"});
  const char* monop = find_font({"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf", "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"});
  ImFont* mono = nullptr;
  if (sans) io.Fonts->AddFontFromFileTTF(sans, 18.0f); else io.Fonts->AddFontDefault();
  if (monop) mono = io.Fonts->AddFontFromFileTTF(monop, 16.0f);
  ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
  ImGui_ImplSDLRenderer2_Init(ren);

  App app(model, o);
  app.mono = mono;
  app.renderer_name = ri.name ? ri.name : "?";
  app.video_driver = SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?";
  app.sandbox = o.auto_sandbox;
  if (!o.scan_dir.empty()) std::snprintf(app.scan_path, sizeof app.scan_path, "%s", o.scan_dir.c_str());
  if (!o.select_id.empty()) app.selected = o.select_id;
  else if (!model.games().empty()) app.selected = model.games()[0].game_id;

  bool quit = false;
  int frame = 0;
  while (!quit) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      ImGui_ImplSDL2_ProcessEvent(&ev);
      if (ev.type == SDL_QUIT) quit = true;
      if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE) quit = true;
    }
    model.poll();
    if (frame == 5 && !o.scan_dir.empty()) app.do_scan();
    if (frame == 10 && !o.auto_launch.empty()) { app.selected = o.auto_launch; app.do_launch(o.auto_launch); }

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    app.frame(static_cast<float>(w), static_cast<float>(h));
    ImGui::Render();
    SDL_SetRenderDrawColor(ren, 19, 20, 26, 255);
    SDL_RenderClear(ren);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
    for (const auto& s : o.shots)
      if (s.first == frame) { if (save_bmp(ren, s.second)) std::fprintf(stderr, "pnr-ui: saved %s (frame %d)\n", s.second.c_str(), frame); else std::fprintf(stderr, "pnr-ui: screenshot failed: %s\n", SDL_GetError()); }
    SDL_RenderPresent(ren);
    ++frame;
    if (o.frames >= 0 && frame >= o.frames) quit = true;
    SDL_Delay(o.frames >= 0 ? 16 : 8);
  }
  model.wait_idle();
  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}
