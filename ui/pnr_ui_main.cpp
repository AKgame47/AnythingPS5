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
#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"
#include "launcher/launcher_core.hpp"
#include "plugin/manifest.hpp"
#include "ui/ui_model.hpp"

namespace {

struct Opts {
  std::string db;
  int w = 1366, h = 768;
  int frames = -1;                                  // quit after N frames (tests, recordings)
  std::vector<std::pair<int, std::string>> shots;   // save a BMP of the window at frame N
  std::string select_id, auto_launch, scan_dir;     // test hooks: same code paths as the mouse
  bool auto_sandbox = false;
  bool open_add = false;
  bool open_store = false;
  int settings_tab = -1;
  std::string page_id;   // open the game page for this id at start (test hook)
  int tab = 0;           // initial tab on the game page (test hook)
};

const ImVec4 kAccent(0.0f, 0.44f, 0.82f, 1.0f);
const ImVec4 kMuted(0.60f, 0.63f, 0.70f, 1.0f);

ImVec4 compat_color(const std::string& s) {
  if (s == "Verified Playable") return ImVec4(0.30f, 0.85f, 0.45f, 1);
  if (s == "Playable With Issues") return ImVec4(0.95f, 0.75f, 0.25f, 1);
  if (s == "Launches Only") return ImVec4(0.40f, 0.65f, 1.00f, 1);
  if (s == "Unsupported") return ImVec4(0.95f, 0.40f, 0.40f, 1);
  if (s == "Blocked by Missing Dependency") return ImVec4(0.95f, 0.55f, 0.25f, 1);
  return ImVec4(0.65f, 0.67f, 0.72f, 1);  // Untested
}

ImVec4 A_(ImVec4 c, float a) { c.w = a; return c; }

void apply_style() {
  ImGui::StyleColorsDark();
  ImGuiStyle& st = ImGui::GetStyle();
  st.WindowRounding = 0; st.ChildRounding = 8; st.FrameRounding = 6; st.GrabRounding = 6; st.PopupRounding = 6;
  st.FramePadding = ImVec2(10, 7); st.ItemSpacing = ImVec2(10, 9); st.CellPadding = ImVec2(10, 8);
  st.WindowPadding = ImVec2(16, 16); st.ScrollbarRounding = 8;
  ImVec4* c = st.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.062f, 0.068f, 0.088f, 1);
  c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.10f, 0.13f, 1);
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
  c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TabHovered] = A_(ImVec4(0.27f, 0.40f, 0.78f, 1), 0.6f);
  c[ImGuiCol_TabSelected] = ImVec4(0.20f, 0.30f, 0.62f, 1);
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
  // ---- game page (UI-2)
  enum Screen { kLibrary = 0, kGame = 1, kStore = 2, kSettings = 3 };
  int screen = kLibrary;
  int want_tab = -1;
  std::string loaded_for;            // game id the page state below belongs to
  int compat_idx = 0;
  char compat_note[220] = "";
  std::string compat_msg; bool compat_ok = false;
  std::string verify_msg; bool verify_ok = false;
  char export_path[400] = "";
  std::string export_msg; bool export_ok = false;
  bool keep_saves = true, have_dry = false;
  pnr::launcher::RemoveOutcome dry;
  std::string remove_msg;

  void open_page(const std::string& id) { selected = id; screen = kGame; loaded_for.clear(); }
  void load_page_state(const pnr::launcher::GameEntry& g) {
    loaded_for = g.game_id;
    const auto& st = pnr::launcher::compat_states();
    compat_idx = 0;
    for (size_t i = 0; i < st.size(); ++i) if (st[i] == g.compatibility) compat_idx = static_cast<int>(i);
    std::snprintf(compat_note, sizeof compat_note, "%s", g.compatibility_notes.c_str());
    compat_msg.clear(); verify_msg.clear(); export_msg.clear(); remove_msg.clear(); have_dry = false;
    std::snprintf(export_path, sizeof export_path, "/tmp/%s-saves.tar", g.game_id.c_str());
  }

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

  bool open_add = false;

  static ImU32 U(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
  static ImVec4 A(ImVec4 c, float a) { c.w = a; return c; }
  // Generated cover colours (no artwork exists for a game, so each cover is a gradient seeded by its id).
  static ImVec4 tint(const std::string& id, float s, float v) {
    unsigned h = 2166136261u;
    for (unsigned char ch : id) { h ^= ch; h *= 16777619u; }
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB((h % 360) / 360.0f, s, v, r, g, b);
    return ImVec4(r, g, b, 1);
  }
  static float chip(ImDrawList* dl, ImVec2 p, const std::string& text, ImVec4 col, float size) {
    ImFont* f = ImGui::GetFont();
    ImVec2 ts = f->CalcTextSizeA(size, 10000.0f, 0.0f, text.c_str());
    float w = ts.x + size + 20, h = size + 10;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), U(A(col, 0.18f)), h / 2);
    dl->AddCircleFilled(ImVec2(p.x + 13, p.y + h / 2), size * 0.28f, U(col));
    ImVec4 t(col.x * 0.55f + 0.45f, col.y * 0.55f + 0.45f, col.z * 0.55f + 0.45f, 1);
    dl->AddText(f, size, ImVec2(p.x + 13 + size * 0.28f + 7, p.y + 5), U(t), text.c_str());
    return w;
  }
  static void text_at(ImDrawList* dl, ImVec2 p, float size, ImVec4 col, const std::string& s, float max_w = 0) {
    ImFont* f = ImGui::GetFont();
    if (max_w > 0) dl->PushClipRect(p, ImVec2(p.x + max_w, p.y + size + 6), true);
    dl->AddText(f, size, p, U(col), s.c_str());
    if (max_w > 0) dl->PopClipRect();
  }

  void rail(float h) {
    ImGui::BeginChild("rail", ImVec2(228, h), false, ImGuiWindowFlags_NoScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(o, ImVec2(o.x + 228, o.y + h), U(ImVec4(0.045f, 0.05f, 0.063f, 1)));
    dl->AddRectFilled(ImVec2(o.x + 24, o.y + 26), ImVec2(o.x + 58, o.y + 60), U(kAccent), 8);
    text_at(dl, ImVec2(o.x + 31, o.y + 29), 28, ImVec4(1, 1, 1, 1), "P");
    text_at(dl, ImVec2(o.x + 70, o.y + 26), 22, ImVec4(0.93f, 0.95f, 1, 1), "PNR");
    text_at(dl, ImVec2(o.x + 70, o.y + 50), 12, kMuted, "Native Runtime");
    struct Item { const char* name; const char* tag; bool on; };
    Item items[] = {{"Library", "", screen == kLibrary}, {"Game page", sel_game() ? "" : "pick a game", screen == kGame}, {"Store", "", screen == kStore}, {"Settings", "", screen == kSettings},
                    {"Hardware", "planned", false}, {"Diagnostics", "planned", false}, {"Reports", "planned", false}};
    float y = 96;
    int idx = 0;
    for (const Item& it : items) {
      ImGui::SetCursorPos(ImVec2(0, y));
      bool clicked = ImGui::InvisibleButton(it.name, ImVec2(228, 40));
      if (clicked && idx == 0) screen = kLibrary;
      if (clicked && idx == 1 && sel_game()) open_page(selected);
      if (clicked && idx == 2) { screen = kStore; store_dirty(); }
      if (clicked && idx == 3) { screen = kSettings; plist_dirty = true; }
      ++idx;
      bool hov = ImGui::IsItemHovered();
      ImVec2 a(o.x, o.y + y), b(o.x + 228, o.y + y + 40);
      if (it.on) {
        dl->AddRectFilled(a, b, U(A(kAccent, 0.14f)));
        dl->AddRectFilled(a, ImVec2(a.x + 4, b.y), U(kAccent));
      } else if (hov) dl->AddRectFilled(a, b, U(ImVec4(1, 1, 1, 0.04f)));
      ImVec4 tc = it.on ? ImVec4(1, 1, 1, 1) : ImVec4(0.52f, 0.55f, 0.63f, 1);
      text_at(dl, ImVec2(a.x + 26, a.y + 10), 18, tc, it.name);
      if (it.tag[0]) {
        ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(12, 1000, 0, it.tag);
        ImVec2 tp(b.x - ts.x - 26, a.y + 13);
        dl->AddRectFilled(ImVec2(tp.x - 7, tp.y - 2), ImVec2(tp.x + ts.x + 7, tp.y + 16), U(ImVec4(1, 1, 1, 0.07f)), 9);
        text_at(dl, tp, 12, ImVec4(0.55f, 0.58f, 0.66f, 1), it.tag);
      }
      y += 42;
    }
    text_at(dl, ImVec2(o.x + 26, o.y + h - 74), 12, kMuted, "Linux x86-64 only");
    text_at(dl, ImVec2(o.x + 26, o.y + h - 56), 12, kMuted, "Windows: Blocked");
    text_at(dl, ImVec2(o.x + 26, o.y + h - 38), 12, kMuted, "Untested on real GPU/display");
    ImGui::EndChild();
  }

  const pnr::launcher::GameEntry* sel_game() const {
    for (const auto& e : model.games()) if (e.game_id == selected) return &e;
    return nullptr;
  }

  void hero(float w, float h) {
    ImGui::BeginChild("hero", ImVec2(w, h), false, ImGuiWindowFlags_NoScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    const auto* g = sel_game();
    ImVec4 base(0.075f, 0.082f, 0.105f, 1);
    if (!g) {
      dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), U(ImVec4(0.09f, 0.1f, 0.13f, 1)));
      text_at(dl, ImVec2(o.x + 40, o.y + h / 2 - 30), 30, ImVec4(0.9f, 0.92f, 1, 1), "Your library is empty");
      text_at(dl, ImVec2(o.x + 40, o.y + h / 2 + 10), 16, kMuted, "PNR only runs packages you add. There is no default source.");
      ImGui::SetCursorPos(ImVec2(40, h / 2 + 44));
      if (primary_button("Add games", ImVec2(170, 42), true)) open_add = true;
      ImGui::EndChild();
      return;
    }
    ImVec4 c1 = tint(g->game_id, 0.62f, 0.50f);
    dl->AddRectFilledMultiColor(o, ImVec2(o.x + w, o.y + h), U(ImVec4(c1.x * 0.8f, c1.y * 0.8f, c1.z * 0.8f, 1)), U(base), U(base), U(ImVec4(c1.x * 0.35f, c1.y * 0.35f, c1.z * 0.35f, 1)));
    dl->AddRectFilledMultiColor(o, ImVec2(o.x + w, o.y + h), U(A(base, 0)), U(A(base, 0)), U(A(base, 1)), U(A(base, 1)));
    // cover
    const float cs = h - 60;
    ImVec2 ca(o.x + 36, o.y + 30);
    cover(dl, ca, cs, *g, 110);
    float tx = o.x + 36 + cs + 32;
    text_at(dl, ImVec2(tx, o.y + 34), 40, ImVec4(1, 1, 1, 1), g->title, w - (tx - o.x) - 24);
    chip(dl, ImVec2(tx, o.y + 90), g->compatibility, compat_color(g->compatibility), 15);
    char meta[256];
    std::snprintf(meta, sizeof meta, "%s   v%s   %lld launch%s   Integrity %s", g->game_id.c_str(), g->version.c_str(), static_cast<long long>(g->launch_count), g->launch_count == 1 ? "" : "es", g->integrity.empty() ? "unknown" : g->integrity.c_str());
    text_at(dl, ImVec2(tx, o.y + 130), 15, ImVec4(0.75f, 0.78f, 0.86f, 1), meta, w - (tx - o.x) - 24);
    text_at(dl, ImVec2(tx, o.y + 152), 13, kMuted, g->compat_human_set ? "Status set by a person." : "Status set automatically. PNR never marks a game Playable on its own.", w - (tx - o.x) - 24);
    std::string blocker = model.launch_blocker(*g);
    ImGui::SetCursorPos(ImVec2(tx - o.x, h - 62));
    if (!blocker.empty()) ImGui::BeginDisabled();
    if (primary_button(model.launching() ? "Running..." : "Launch", ImVec2(190, 46), false)) do_launch(g->game_id);
    if (!blocker.empty()) ImGui::EndDisabled();
    ImGui::SameLine(0, 14);
    ImGui::SetCursorPosY(h - 62);
    if (ImGui::Button("Details", ImVec2(110, 46))) open_page(g->game_id);
    ImGui::SameLine(0, 20);
    ImGui::SetCursorPosY(h - 52);
    ImGui::Checkbox("Run in sandbox", &sandbox);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Linux namespaces, seccomp denylist and a private root.\nPARTIAL: not a complete sandbox. Refuses to start if unusable.");
    if (!blocker.empty()) {
      ImGui::SameLine(0, 20);
      ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "%s", blocker.c_str());
    }
    ImGui::EndChild();
  }

  bool primary_button(const char* label, ImVec2 size, bool) {
    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.55f, 0.95f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.36f, 0.70f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, size.y / 2);
    ImGui::PushFont(nullptr, 18.0f);
    bool r = ImGui::Button(label, size);
    ImGui::PopFont();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    return r;
  }

  static void cover(ImDrawList* dl, ImVec2 a, float size, const pnr::launcher::GameEntry& g, float letter) {
    ImVec2 b(a.x + size, a.y + size);
    ImVec4 c1 = tint(g.game_id, 0.60f, 0.62f), c2 = tint(g.game_id + "~", 0.70f, 0.30f);
    dl->AddRectFilledMultiColor(a, b, U(c1), U(ImVec4(c1.x * 0.7f, c1.y * 0.7f, c1.z * 0.7f, 1)), U(c2), U(ImVec4(c2.x * 1.2f, c2.y * 1.2f, c2.z * 1.2f, 1)));
    std::string ini = g.title.empty() ? "?" : g.title.substr(0, 1);
    ImFont* f = ImGui::GetFont();
    ImVec2 ts = f->CalcTextSizeA(letter, 1000, 0, ini.c_str());
    dl->AddText(f, letter, ImVec2(a.x + (size - ts.x) / 2, a.y + (size - ts.y) / 2 - 4), U(ImVec4(1, 1, 1, 0.82f)), ini.c_str());
  }

  void grid(float w, float h, const std::vector<size_t>& idx) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("grid", ImVec2(w, h), false);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cw = 176, cover_h = 176, ch = 246, gap = 20, pad = 36;
    int cols = std::max(1, static_cast<int>((w - pad * 2 + gap) / (cw + gap)));
    int n = 0;
    int rows = 0;
    for (size_t i : idx) {
      const auto& g = model.games()[i];
      int r = n / cols, c = n % cols;
      ++n;
      rows = r + 1;
      ImGui::SetCursorPos(ImVec2(pad + c * (cw + gap), 8 + r * (ch + gap)));
      ImVec2 p = ImGui::GetCursorScreenPos();
      ImGui::PushID(g.game_id.c_str());
      if (ImGui::InvisibleButton("card", ImVec2(cw, ch))) selected = g.game_id;
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) open_page(g.game_id);
      bool hov = ImGui::IsItemHovered(), sel = g.game_id == selected;
      ImGui::PopID();
      ImVec2 a(p.x, p.y + (hov ? -3.0f : 0.0f)), b(a.x + cw, a.y + ch);
      dl->AddRectFilled(a, b, U(ImVec4(0.105f, 0.115f, 0.145f, 1)), 4);
      cover(dl, a, cw, g, 76);
      text_at(dl, ImVec2(a.x + 12, a.y + cover_h + 10), 17, ImVec4(0.95f, 0.96f, 1, 1), g.title, cw - 24);
      chip(dl, ImVec2(a.x + 8, a.y + cover_h + 36), g.compatibility, compat_color(g.compatibility), 12);
      if (sel) dl->AddRect(ImVec2(a.x - 2, a.y - 2), ImVec2(b.x + 2, b.y + 2), U(kAccent), 5, 0, 3.0f);
      else if (hov) dl->AddRect(a, b, U(ImVec4(1, 1, 1, 0.28f)), 4, 0, 1.5f);
    }
    ImGui::SetCursorPos(ImVec2(0, 8 + rows * (ch + gap)));
    ImGui::Dummy(ImVec2(1, 1));
    ImGui::EndChild();
    ImGui::PopStyleColor();
  }

  void output_panel(float w) {
    auto r = model.last_launch();
    const auto* g = sel_game();
    if (!g || r.game_id != g->game_id || !(r.running || r.finished)) return;
    ImGui::BeginChild("out", ImVec2(w, 118), false, ImGuiWindowFlags_NoScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(o, ImVec2(o.x + w, o.y + 118), U(ImVec4(0.04f, 0.045f, 0.057f, 1)));
    ImVec4 col = r.running ? kAccent : (r.clean ? ImVec4(0.35f, 0.85f, 0.5f, 1) : ImVec4(0.95f, 0.45f, 0.4f, 1));
    dl->AddRectFilled(o, ImVec2(o.x + 4, o.y + 118), U(col));
    text_at(dl, ImVec2(o.x + 20, o.y + 8), 15, col, r.summary + (r.sandbox ? "  [sandbox requested]" : ""), w - 40);
    ImGui::SetCursorPos(ImVec2(20, 32));
    ImGui::BeginChild("outtxt", ImVec2(w - 40, 80), false);
    if (mono) ImGui::PushFont(mono, 14.0f);
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(ImVec4(0.72f, 0.76f, 0.84f, 1), "%s", r.output.empty() ? "(no output yet)" : r.output.c_str());
    ImGui::PopTextWrapPos();
    if (mono) ImGui::PopFont();
    ImGui::EndChild();
    ImGui::EndChild();
  }

  void add_popup() {
    if (open_add) { ImGui::OpenPopup("Add games"); open_add = false; }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Size.x / 2, vp->Size.y / 2), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(780.0f, vp->Size.x - 80), 420), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28, 24));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10);
    if (ImGui::BeginPopupModal("Add games", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
      ImGui::PushFont(nullptr, 26.0f);
      ImGui::TextUnformatted("Add games");
      ImGui::PopFont();
      ImGui::TextColored(kMuted, "Scans a folder for .pnrgame packages. Read-only: nothing runs until you press Launch.");
      ImGui::Dummy(ImVec2(0, 6));
      float w = ImGui::GetContentRegionAvail().x;
      ImGui::SetNextItemWidth(w - 150);
      bool enter = ImGui::InputTextWithHint("##scanpath", "Folder, for example /home/you/games", scan_path, sizeof scan_path, ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::SameLine();
      if (ImGui::Button("Scan folder", ImVec2(140, 0)) || enter) do_scan();
      if (!scan_msg.empty()) ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "%s", scan_msg.c_str());
      if (!import_msg.empty()) ImGui::TextColored(import_ok ? ImVec4(0.35f, 0.85f, 0.5f, 1) : ImVec4(0.95f, 0.5f, 0.4f, 1), "%s", import_msg.c_str());
      ImGui::BeginChild("scanres", ImVec2(0, -44), false);
      if (scanned) {
        if (scan_report.items.empty()) ImGui::TextColored(kMuted, "No .pnrgame packages or ELF files found in that folder.");
        for (const auto& it : scan_report.items) {
          if (it.kind == "pnrgame") {
            if (it.ok) {
              ImGui::Text("%s   %s %s", it.file.c_str(), it.title.c_str(), it.version.c_str());
              ImGui::SameLine();
              if (it.in_library) ImGui::TextColored(kMuted, "already in library");
              else if (ImGui::Button(("Import##" + it.file).c_str())) do_import(scan_path, it.file);
            } else ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "%s  not importable: %s", it.file.c_str(), it.error.c_str());
          } else {
            ImGui::TextColored(kMuted, "%s  ELF program (%s). Package it first.", it.file.c_str(), it.ok ? "valid x86-64" : it.error.c_str());
          }
        }
        if (scan_report.skipped) ImGui::TextColored(kMuted, "%d other files skipped.", scan_report.skipped);
      }
      ImGui::EndChild();
      if (ImGui::Button("Close", ImVec2(110, 0))) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
  }

  void status_bar(float W, float H) {
    const float status_h = 26;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, H - status_h), ImVec2(W, H), U(ImVec4(0.04f, 0.044f, 0.055f, 1)));
    char sb[512];
    std::snprintf(sb, sizeof sb, "Library %s     video %s, renderer %s     Windows: Blocked     untested on a real display/GPU", model.db_path().c_str(), video_driver.c_str(), renderer_name.c_str());
    text_at(dl, ImVec2(16, H - status_h + 5), 13, kMuted, sb, W - 32);
  }
  static std::string fmt_time(double at) {
    time_t t = static_cast<time_t>(at);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char b[64];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tmv);
    return b;
  }
  static void kv_row(const char* k, const std::string& v, bool mono_font = false, ImFont* mono = nullptr) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(kMuted, "%s", k);
    ImGui::TableSetColumnIndex(1);
    if (mono_font && mono) ImGui::PushFont(mono, 15.0f);
    ImGui::TextWrapped("%s", v.empty() ? "-" : v.c_str());
    if (mono_font && mono) ImGui::PopFont();
  }
  static void note_line(const std::string& msg, bool ok) {
    if (msg.empty()) return;
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(ok ? ImVec4(0.35f, 0.85f, 0.5f, 1) : ImVec4(0.95f, 0.5f, 0.4f, 1), "%s", msg.c_str());
    ImGui::PopTextWrapPos();
  }

  void game_page(float mw, float body_h, float rail_w) {
    const auto* gp = sel_game();
    pnr::launcher::GameEntry g = *gp;
    if (loaded_for != g.game_id) load_page_state(g);
    // header
    ImGui::SetCursorPos(ImVec2(rail_w + 36, 18));
    if (ImGui::Button("< Library", ImVec2(110, 0))) screen = kLibrary;
    ImGui::SameLine(0, 14);
    ImGui::SetCursorPosY(24);
    ImGui::TextColored(kMuted, "Library  /  %s", g.title.c_str());
    ImGui::SetCursorPos(ImVec2(rail_w, 60));
    const float hh = 200;
    ImGui::BeginChild("ghead", ImVec2(mw, hh), false, ImGuiWindowFlags_NoScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImVec4 base(0.075f, 0.082f, 0.105f, 1);
    ImVec4 c1 = tint(g.game_id, 0.62f, 0.50f);
    dl->AddRectFilledMultiColor(o, ImVec2(o.x + mw, o.y + hh), U(ImVec4(c1.x * 0.8f, c1.y * 0.8f, c1.z * 0.8f, 1)), U(base), U(base), U(ImVec4(c1.x * 0.35f, c1.y * 0.35f, c1.z * 0.35f, 1)));
    const float cs = hh - 40;
    cover(dl, ImVec2(o.x + 36, o.y + 20), cs, g, 90);
    float tx = o.x + 36 + cs + 28, tw = mw - (tx - o.x) - 24;
    text_at(dl, ImVec2(tx, o.y + 18), 36, ImVec4(1, 1, 1, 1), g.title, tw);
    chip(dl, ImVec2(tx, o.y + 68), g.compatibility, compat_color(g.compatibility), 15);
    char meta[256];
    std::snprintf(meta, sizeof meta, "%s   v%s   %s   %lld launch%s", g.game_id.c_str(), g.version.c_str(), g.platform.empty() ? "platform unknown" : g.platform.c_str(), static_cast<long long>(g.launch_count), g.launch_count == 1 ? "" : "es");
    text_at(dl, ImVec2(tx, o.y + 106), 15, ImVec4(0.75f, 0.78f, 0.86f, 1), meta, tw);
    std::string blocker = model.launch_blocker(g);
    ImGui::SetCursorPos(ImVec2(tx - o.x, hh - 62));
    if (!blocker.empty()) ImGui::BeginDisabled();
    if (primary_button(model.launching() ? "Running..." : "Launch", ImVec2(190, 46), false)) do_launch(g.game_id);
    if (!blocker.empty()) ImGui::EndDisabled();
    ImGui::SameLine(0, 20);
    ImGui::SetCursorPosY(hh - 52);
    ImGui::Checkbox("Run in sandbox", &sandbox);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Linux namespaces, seccomp denylist and a private root.\nPARTIAL: not a complete sandbox. Refuses to start if unusable.");
    if (!blocker.empty()) { ImGui::SameLine(0, 20); ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "%s", blocker.c_str()); }
    ImGui::EndChild();
    float y = 60 + hh;
    auto r = model.last_launch();
    if (r.game_id == g.game_id && (r.running || r.finished)) { ImGui::SetCursorPos(ImVec2(rail_w, y)); output_panel(mw); y += 118; }
    // tabs
    const float foot_h = 56;
    ImGui::SetCursorPos(ImVec2(rail_w + 36, y + 14));
    ImGui::BeginChild("gtabs", ImVec2(mw - 72, body_h - y - 14 - foot_h), false);
    if (ImGui::BeginTabBar("gt")) {
      auto tab_flags = [&](int i) { return want_tab == i ? ImGuiTabItemFlags_SetSelected : 0; };
      if (ImGui::BeginTabItem("Overview", nullptr, tab_flags(0))) { tab_overview(g); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("Compatibility", nullptr, tab_flags(1))) { tab_compat(g); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("History", nullptr, tab_flags(2))) { tab_history(g); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("Manage", nullptr, tab_flags(3))) { tab_manage(g); ImGui::EndTabItem(); }
      ImGui::EndTabBar();
    }
    want_tab = -1;
    ImGui::EndChild();
    // honesty footer
    ImGui::SetCursorPos(ImVec2(rail_w + 36, body_h - foot_h + 8));
    ImGui::PushTextWrapPos(rail_w + mw - 36);
    ImGui::TextColored(kMuted, "State on the road: PNR runs developer-built x86-64 packages natively. Status is never set to Playable automatically. The sandbox is partial isolation. Verify re-checks file presence and the ELF header only, not payload checksums.");
    ImGui::PopTextWrapPos();
  }


  // ---- store screen (UI-3)
  std::vector<pnr::ui::StoreRow> srows;
  int srows_age = 1000;
  char ssearch[96] = "";
  char add_id[48] = "", add_url[300] = "", add_rights[300] = "";
  std::string add_msg; bool add_ok = false;
  bool open_src = false;
  std::string store_msg; bool store_msg_ok = false;
  std::string install_scan, pending_remove;
  bool store_open_hook = false;

  void store_dirty() { srows_age = 1000; }
  static const char* human_size(long long b, char* buf, size_t n) {
    if (b >= (1LL << 20)) std::snprintf(buf, n, "%.1f MiB", b / 1048576.0);
    else if (b >= 1024) std::snprintf(buf, n, "%.1f KiB", b / 1024.0);
    else std::snprintf(buf, n, "%lld B", b);
    return buf;
  }

  void store_page(float mw, float body_h, float rail_w) {
    if (srows_age++ > 30) { srows = model.store_rows("", ssearch); srows_age = 0; }
    ImGui::Dummy(ImVec2(mw, 22));
    ImGui::SetCursorPosX(rail_w + 36);
    ImGui::PushFont(nullptr, 30.0f); ImGui::TextUnformatted("Store"); ImGui::PopFont();
    ImGui::SameLine(0, 22);
    ImGui::SetCursorPosY(24);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 18);
    ImGui::SetNextItemWidth(340);
    if (ImGui::InputTextWithHint("##ssearch", "Search name, id, category", ssearch, sizeof ssearch)) store_dirty();
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(rail_w + mw - 36 - 130 - 10 - 150);
    ImGui::SetCursorPosY(24);
    auto job = model.store_job();
    if (job.running) ImGui::BeginDisabled();
    if (ImGui::Button("Refresh all", ImVec2(140, 0))) { model.store_start_refresh(""); store_msg.clear(); }
    if (job.running) ImGui::EndDisabled();
    ImGui::SameLine(0, 10);
    ImGui::SetCursorPosY(24);
    if (primary_button("Add source", ImVec2(130, 34), false)) open_src = true;
    ImGui::PopStyleVar();
    // honesty banner
    ImGui::SetCursorPos(ImVec2(rail_w + 36, 72));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.12f, 0.08f, 1));
    ImGui::BeginChild("sbanner", ImVec2(mw - 72, 54), false, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos(ImVec2(14, 8));
    ImGui::PushTextWrapPos(mw - 72 - 14);
    ImGui::TextColored(ImVec4(0.92f, 0.82f, 0.5f, 1), "No default source: PNR lists only sources you add. The rights statement is yours; PNR cannot verify it. Only pnr-package files install, every download is hash-checked and scanned, and a flagged scan refuses the install.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    float y = 72 + 54 + 12;
    const auto& srcs = model.store_sources();
    if (srcs.empty()) {
      ImGui::SetCursorPos(ImVec2(rail_w + 36, y + 70));
      ImGui::PushFont(nullptr, 26.0f); ImGui::TextUnformatted("No sources yet"); ImGui::PopFont();
      ImGui::SetCursorPosX(rail_w + 36);
      ImGui::TextColored(kMuted, "Add the https index URL of a source you have the right to use, and say why.");
      ImGui::SetCursorPosX(rail_w + 36);
      ImGui::Dummy(ImVec2(0, 6));
      ImGui::SetCursorPosX(rail_w + 36);
      if (primary_button("Add source", ImVec2(170, 42), true)) open_src = true;
    } else {
      // sources strip
      ImGui::SetCursorPos(ImVec2(rail_w + 36, y));
      for (const auto& s : srcs) {
        ImGui::PushID(s.id.c_str());
        ImGui::BeginGroup();
        ImGui::TextColored(ImVec4(0.9f, 0.92f, 1, 1), "%s", s.id.c_str());
        ImGui::SameLine(0, 8);
        ImGui::TextColored(kMuted, "%s%s", s.host.c_str(), s.pubkey.empty() ? "" : "  (key pinned)");
        ImGui::SameLine(0, 14);
        if (ImGui::SmallButton("Refresh")) { model.store_start_refresh(s.id); store_msg.clear(); }
        ImGui::SameLine(0, 6);
        if (pending_remove == s.id) {
          if (ImGui::SmallButton("Confirm remove")) { bool ok = false; store_msg = model.store_remove_source(s.id, true, &ok); store_msg_ok = ok; pending_remove.clear(); store_dirty(); }
          ImGui::SameLine(0, 6);
          if (ImGui::SmallButton("Keep")) pending_remove.clear();
        } else if (ImGui::SmallButton("Remove")) pending_remove = s.id;
        ImGui::EndGroup();
        ImGui::PopID();
        ImGui::SameLine(0, 36);
      }
      ImGui::NewLine();
      ImGui::SetCursorPosX(rail_w + 36);
      ImGui::TextColored(kMuted, "Declared rights (unverified): %s", srcs[0].rights.c_str());
      y = ImGui::GetCursorPosY() + 10;
      // package list
      const float job_h = 44;
      ImGui::SetCursorPos(ImVec2(rail_w + 36, y));
      ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
      ImGui::BeginChild("spk", ImVec2(mw - 72, body_h - y - job_h - 4), false);
      ImDrawList* dl = ImGui::GetWindowDrawList();
      if (srows.empty()) ImGui::TextColored(kMuted, "No packages listed. Press Refresh to fetch the source's index.");
      const float rw = mw - 72 - 14, rh = 92;
      int n = 0;
      for (const auto& r : srows) {
        ImGui::SetCursorPos(ImVec2(0, 4 + n * (rh + 10)));
        ++n;
        ImVec2 a = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(a, ImVec2(a.x + rw, a.y + rh), U(ImVec4(0.105f, 0.115f, 0.145f, 1)), 6);
        pnr::launcher::GameEntry fake; fake.game_id = r.id; fake.title = r.name;
        cover(dl, ImVec2(a.x + 10, a.y + 10), rh - 20, fake, 40);
        float tx = a.x + rh + 8;
        text_at(dl, ImVec2(tx, a.y + 10), 19, ImVec4(0.95f, 0.96f, 1, 1), r.name + "  v" + r.version, rw - rh - 330);
        char sz[32]; human_size(r.size, sz, sizeof sz);
        text_at(dl, ImVec2(tx, a.y + 36), 14, kMuted, r.category + "   " + r.license + "   " + sz + "   " + r.source + "/" + r.id, rw - rh - 330);
        text_at(dl, ImVec2(tx, a.y + 58), 14, ImVec4(0.7f, 0.73f, 0.8f, 1), r.description, rw - rh - 330);
        ImVec4 lc = r.label == "supported" ? ImVec4(0.35f, 0.85f, 0.5f, 1) : r.label == "experimental" ? ImVec4(0.95f, 0.75f, 0.25f, 1) : ImVec4(0.95f, 0.5f, 0.4f, 1);
        chip(dl, ImVec2(a.x + rw - 320, a.y + 12), r.label, lc, 13);
        if (!r.status.empty()) chip(dl, ImVec2(a.x + rw - 320, a.y + 50), r.status, kAccent, 13);
        ImGui::SetCursorPos(ImVec2(rw - 150, 4 + (n - 1) * (rh + 10) + 24));
        ImGui::PushID((r.source + "/" + r.id).c_str());
        bool busy = job.running;
        if (r.label == "unsupported") { ImGui::BeginDisabled(); ImGui::Button("Unsupported", ImVec2(130, 40)); ImGui::EndDisabled(); }
        else if (r.status == "installed") { ImGui::BeginDisabled(); ImGui::Button("Installed", ImVec2(130, 40)); ImGui::EndDisabled(); }
        else if (r.status == "verified") {
          if (busy) ImGui::BeginDisabled();
          if (primary_button("Install", ImVec2(130, 40), false)) {
            bool ok = false; store_msg = model.store_install(r.source, r.id, &ok, &install_scan); store_msg_ok = ok; store_dirty();
          }
          if (busy) ImGui::EndDisabled();
        } else {
          if (busy) ImGui::BeginDisabled();
          if (primary_button(r.status.empty() ? "Get" : "Resume", ImVec2(130, 40), false)) { model.store_start_get(r.source, r.id); store_msg.clear(); }
          if (busy) ImGui::EndDisabled();
        }
        ImGui::PopID();
      }
      ImGui::SetCursorPos(ImVec2(0, 4 + n * (rh + 10)));
      ImGui::Dummy(ImVec2(1, 1));
      ImGui::EndChild();
      ImGui::PopStyleColor();
      // job / message strip
      ImGui::SetCursorPos(ImVec2(rail_w + 36, body_h - job_h));
      ImGui::PushTextWrapPos(rail_w + mw - 36);
      std::string line = store_msg;
      ImVec4 col = store_msg_ok ? ImVec4(0.35f, 0.85f, 0.5f, 1) : ImVec4(0.95f, 0.5f, 0.4f, 1);
      if (line.empty() && (job.running || job.finished)) {
        line = job.what + ": " + job.summary;
        col = job.running ? kAccent : (job.ok ? ImVec4(0.35f, 0.85f, 0.5f, 1) : ImVec4(0.95f, 0.5f, 0.4f, 1));
      }
      while (!line.empty() && (line.back() == '\n' || line.back() == ' ')) line.pop_back();
      // the outcome is the last line that is not a trailing "(...)" hint
      for (;;) {
        size_t nl = line.rfind('\n');
        if (nl == std::string::npos) break;
        std::string last = line.substr(nl + 1);
        size_t f = last.find_first_not_of(' ');
        if (f != std::string::npos && last[f] == '(') { line.resize(nl); continue; }
        line = last.substr(f == std::string::npos ? 0 : f);
        break;
      }
      ImGui::TextColored(col, "%s", line.c_str());
      ImGui::PopTextWrapPos();
    }
    if (job.finished && !job.running && last_job_what != job.what + job.summary) { last_job_what = job.what + job.summary; store_dirty(); }
    src_popup();
  }
  std::string last_job_what;

  void src_popup() {
    if (open_src) { ImGui::OpenPopup("Add source"); open_src = false; }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Size.x / 2, vp->Size.y / 2), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(760.0f, vp->Size.x - 80), 400), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28, 24));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10);
    if (ImGui::BeginPopupModal("Add source", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
      ImGui::PushFont(nullptr, 26.0f); ImGui::TextUnformatted("Add source"); ImGui::PopFont();
      ImGui::TextColored(kMuted, "Nothing is fetched until you press Refresh. PNR cannot check your rights statement.");
      ImGui::Dummy(ImVec2(0, 6));
      ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##aid", "Short id, for example mygames", add_id, sizeof add_id);
      ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##aurl", "https index URL", add_url, sizeof add_url);
      ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##arights", "Rights statement, for example: these are my own games", add_rights, sizeof add_rights);
      if (primary_button("Add", ImVec2(120, 40), false)) {
        bool ok = false; add_msg = model.store_add_source(add_id, add_url, add_rights, &ok); add_ok = ok;
        if (ok) { add_id[0] = add_url[0] = add_rights[0] = 0; store_dirty(); ImGui::CloseCurrentPopup(); }
      }
      ImGui::SameLine(); if (ImGui::Button("Cancel", ImVec2(110, 40))) ImGui::CloseCurrentPopup();
      if (!add_msg.empty() && !add_ok) { std::string m = add_msg.substr(0, add_msg.find('\n')); ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "%s", m.c_str()); ImGui::PopTextWrapPos(); }
      ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
  }


  // ---- settings screen (UI-4): Plugins, Trust, General
  std::vector<pnr::plugin::InstalledPlugin> plist;
  bool plist_dirty = true;
  std::map<std::string, std::vector<char>> grant_sel;   // per plugin id, one flag per declared permission
  char plug_dir[400] = "";
  std::string plug_msg; bool plug_ok = false;
  std::vector<std::string> plug_notes;
  std::string plug_out;                  // last run output
  std::string pending_plugin_remove;
  char key_name[64] = "", key_hex[100] = "";
  std::string key_msg; bool key_ok = false;
  int set_tab_want = -1;

  static std::string ver_str(const pnr::plugin::Version& v) { return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch); }
  void plugins_tab() {
    if (plist_dirty) { plist = model.plugin_list(); plist_dirty = false; }
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextColored(kMuted, "Plugins are untrusted code. Installing grants nothing: you tick each permission when you enable one.");
    ImGui::SetNextItemWidth(560);
    ImGui::InputTextWithHint("##pdir", "Folder with pnr-plugin.json, for example /home/you/plugins/hello-catalog", plug_dir, sizeof plug_dir);
    ImGui::SameLine();
    if (primary_button("Install", ImVec2(130, 36), false)) {
      auto r = model.plugin_install(plug_dir);
      plug_ok = r.ok; plug_notes.clear();
      if (r.ok) {
        plug_msg = std::string(r.updated ? "Updated " : "Installed ") + r.manifest.name + " " + ver_str(r.manifest.version) + ". Signature: " + r.signature + (r.disabled_by_update ? ". Disabled: the update asks for permissions you have not granted." : ". Not enabled yet.");
        for (auto& w : r.warnings) plug_notes.push_back("analysis: " + w);
      } else {
        plug_msg = "Not installed:";
        for (auto& e : r.errors) plug_notes.push_back(e);
      }
      plist_dirty = true;
    }
    note_line(plug_msg, plug_ok);
    for (auto& n : plug_notes) ImGui::TextColored(kMuted, "  %s", n.c_str());
    ImGui::Dummy(ImVec2(0, 8));
    if (plist.empty()) { ImGui::TextColored(kMuted, "No plugins installed."); return; }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.125f, 0.138f, 0.175f, 1));
    for (const auto& p : plist) {
      const auto& m = p.manifest;
      ImGui::PushID(m.id.c_str());
      ImGui::BeginChild("pc", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
      ImGui::PushFont(nullptr, 21.0f); ImGui::TextUnformatted(m.name.c_str()); ImGui::PopFont();
      ImGui::SameLine(0, 12);
      ImGui::TextColored(kMuted, "v%s   %s", ver_str(m.version).c_str(), m.id.c_str());
      ImGui::SameLine(0, 16);
      ImGui::TextColored(p.enabled ? ImVec4(0.35f, 0.85f, 0.5f, 1) : ImVec4(0.65f, 0.67f, 0.72f, 1), "%s", p.enabled ? "ENABLED" : "disabled");
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(ImVec4(0.75f, 0.78f, 0.86f, 1), "%s", m.description.c_str());
      std::string kinds;
      for (auto k : m.kinds) kinds += std::string(kinds.empty() ? "" : ", ") + pnr::plugin::kind_name(k);
      ImGui::TextColored(kMuted, "kinds: %s     entry: %s     author: %s     license: %s", kinds.c_str(), m.entry_type.c_str(), m.author.c_str(), m.license.c_str());
      ImGui::PopTextWrapPos();
      auto& sel = grant_sel[m.id];
      if (sel.size() != m.permissions.size()) sel.assign(m.permissions.size(), 0);
      if (m.permissions.empty()) ImGui::TextColored(kMuted, "Asks for no permissions.");
      else {
        ImGui::TextColored(kMuted, p.enabled ? "Granted permissions" : "Grant on enable");
        for (size_t i = 0; i < m.permissions.size(); ++i) {
          bool granted = std::find(p.granted.begin(), p.granted.end(), m.permissions[i]) != p.granted.end();
          if (p.enabled) { ImGui::TextColored(granted ? ImVec4(0.35f, 0.85f, 0.5f, 1) : kMuted, "  %s  %s", granted ? "granted:" : "not granted:", m.permissions[i].c_str()); }
          else { bool v = sel[i] != 0; if (ImGui::Checkbox(m.permissions[i].c_str(), &v)) sel[i] = v ? 1 : 0; }
        }
      }
      ImGui::Dummy(ImVec2(0, 4));
      if (!p.enabled) {
        if (primary_button("Enable", ImVec2(130, 38), false)) {
          std::vector<std::string> g;
          for (size_t i = 0; i < m.permissions.size(); ++i) if (sel[i]) g.push_back(m.permissions[i]);
          bool ok = false; plug_msg = model.plugin_enable(m.id, g, &ok); plug_ok = ok; plug_notes.clear(); plist_dirty = true;
        }
      } else {
        if (ImGui::Button("Disable", ImVec2(110, 38))) { bool ok = false; plug_msg = model.plugin_disable(m.id, &ok); plug_ok = ok; plug_notes.clear(); plist_dirty = true; }
        if (m.entry_type == "process") {
          ImGui::SameLine(0, 10);
          if (primary_button("Run", ImVec2(100, 38), false)) {
            std::string why;
            auto rr = model.plugin_run(m.id, &why);
            plug_out.clear();
            if (!rr.started) plug_out = "Not run: " + why;
            else {
              for (auto& l : rr.logs) plug_out += "plugin log: " + l + "\n";
              plug_out += rr.completed ? "result: " + rr.result : "failed: " + rr.error;
              plug_out += "\nbroker: " + std::to_string(rr.served) + " served, " + std::to_string(rr.denied) + " denied (partial sandbox, not a security boundary)";
            }
          }
        }
      }
      ImGui::SameLine(0, 10);
      if (pending_plugin_remove == m.id) {
        if (ImGui::Button("Confirm remove", ImVec2(150, 38))) { bool ok = false; plug_msg = model.plugin_remove(m.id, &ok); plug_ok = ok; plug_notes.clear(); pending_plugin_remove.clear(); plist_dirty = true; }
        ImGui::SameLine(0, 8);
        if (ImGui::Button("Keep", ImVec2(80, 38))) pending_plugin_remove.clear();
      } else if (ImGui::Button("Remove", ImVec2(110, 38))) pending_plugin_remove = m.id;
      ImGui::EndChild();
      ImGui::PopID();
      ImGui::Dummy(ImVec2(0, 8));
    }
    ImGui::PopStyleColor();
    if (!plug_out.empty()) {
      ImGui::PushFont(mono, 15.0f); ImGui::PushTextWrapPos(0);
      ImGui::TextColored(ImVec4(0.72f, 0.76f, 0.84f, 1), "%s", plug_out.c_str());
      ImGui::PopTextWrapPos(); ImGui::PopFont();
    }
  }

  void trust_tab() {
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::Checkbox("Require a valid signature to install a plugin (this session)", &model.plugin_require_signed);
    ImGui::TextColored(kMuted, "A signature from a pinned key proves who made a plugin, not that it is safe. Unsigned plugins stay untrusted either way.");
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::PushFont(nullptr, 20.0f); ImGui::TextUnformatted("Pinned keys"); ImGui::PopFont();
    auto keys = model.plugin_keys();
    if (keys.empty()) ImGui::TextColored(kMuted, "None pinned.");
    for (const auto& k : keys) {
      ImGui::PushID(k.name.c_str());
      ImGui::Text("%s", k.name.c_str());
      ImGui::SameLine(0, 14);
      ImGui::PushFont(mono, 14.0f); ImGui::TextColored(kMuted, "%s", k.pubkey_hex.c_str()); ImGui::PopFont();
      ImGui::SameLine(0, 14);
      if (ImGui::SmallButton("Unpin")) { bool ok = false; key_msg = model.plugin_unpin(k.name, &ok); key_ok = ok; }
      ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::SetNextItemWidth(180); ImGui::InputTextWithHint("##kn", "Name", key_name, sizeof key_name);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(560); ImGui::InputTextWithHint("##kh", "Ed25519 public key, 64 hex characters", key_hex, sizeof key_hex);
    ImGui::SameLine();
    if (ImGui::Button("Pin key", ImVec2(110, 0))) { bool ok = false; key_msg = model.plugin_pin(key_name, key_hex, &ok); key_ok = ok; if (ok) { key_name[0] = key_hex[0] = 0; } }
    note_line(key_msg, key_ok);
  }

  void general_tab() {
    ImGui::Dummy(ImVec2(0, 6));
    if (ImGui::BeginTable("gen", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX)) {
      ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 230);
      ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
      kv_row("Library database", model.db_path(), true, mono);
      kv_row("Store folder", pnr::store::store_dir_for(model.db_path()), true, mono);
      kv_row("Plugin folder", model.plugin_home(), true, mono);
      kv_row("Games in library", std::to_string(model.games().size()));
      kv_row("Store sources", std::to_string(model.store_sources().size()));
      kv_row("Display", video_driver + ", renderer " + renderer_name);
      ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::Checkbox("Launch games in the sandbox by default (this session)", &sandbox);
    ImGui::TextColored(kMuted, "Sandbox: Linux namespaces + seccomp denylist + private root. PARTIAL isolation, no CPU or memory limits.");
    ImGui::Dummy(ImVec2(0, 10));
    ImGui::PushFont(nullptr, 20.0f); ImGui::TextUnformatted("State of the road"); ImGui::PopFont();
    if (ImGui::BeginTable("road", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX)) {
      ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 330);
      ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
      kv_row("Native x86-64 ELF/package launch", "Implemented, tested (Linux)");
      kv_row("PS5 titles via translation", "Planned: the destination, not working today");
      kv_row("Plugins and store", "Implemented, tested; sources and plugins are untrusted");
      kv_row("Real GPU / display", "Untested (software renderer under Xvfb)");
      kv_row("Windows", "Blocked");
      ImGui::EndTable();
    }
    ImGui::TextColored(kMuted, "Settings here last for this session. Nothing is written to disk by this tab.");
  }

  void settings_page(float mw, float body_h, float rail_w) {
    ImGui::Dummy(ImVec2(mw, 22));
    ImGui::SetCursorPosX(rail_w + 36);
    ImGui::PushFont(nullptr, 30.0f); ImGui::TextUnformatted("Settings"); ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(rail_w + 36, 72));
    const float foot_h = 46;
    ImGui::BeginChild("stabs", ImVec2(mw - 72, body_h - 72 - foot_h), false);
    if (ImGui::BeginTabBar("st")) {
      auto fl = [&](int i) { return set_tab_want == i ? ImGuiTabItemFlags_SetSelected : 0; };
      if (ImGui::BeginTabItem("Plugins", nullptr, fl(0))) { plugins_tab(); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("Trust", nullptr, fl(1))) { trust_tab(); ImGui::EndTabItem(); }
      if (ImGui::BeginTabItem("General", nullptr, fl(2))) { general_tab(); ImGui::EndTabItem(); }
      ImGui::EndTabBar();
    }
    set_tab_want = -1;
    ImGui::EndChild();
    ImGui::SetCursorPos(ImVec2(rail_w + 36, body_h - foot_h + 8));
    ImGui::PushTextWrapPos(rail_w + mw - 36);
    ImGui::TextColored(kMuted, "Plugins run only in the partial sandbox and are never trusted by default. Run blocks this window until the plugin finishes or times out (10 s).");
    ImGui::PopTextWrapPos();
  }

  void tab_overview(const pnr::launcher::GameEntry& g) {
    ImGui::Dummy(ImVec2(0, 6));
    if (ImGui::BeginTable("kv", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX)) {
      ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 190);
      ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
      kv_row("Game id", g.game_id, true, mono);
      kv_row("Title", g.title);
      kv_row("Version", g.version);
      kv_row("Platform", g.platform);
      kv_row("Installed", g.installed ? "yes" : "no");
      kv_row("Install path", g.install_path, true, mono);
      kv_row("Entry point", g.executable, true, mono);
      kv_row("Integrity", g.integrity.empty() ? "unknown" : g.integrity);
      kv_row("Can launch", g.runnable ? "yes" : "no: " + g.not_runnable_reason);
      kv_row("Launches", std::to_string(g.launch_count));
      kv_row("Status set by", g.compat_human_set ? "a person" : "PNR bookkeeping (Untested or Launches Only only)");
      ImGui::EndTable();
    }
  }

  void tab_compat(const pnr::launcher::GameEntry& g) {
    ImGui::Dummy(ImVec2(0, 6));
    const auto& st = pnr::launcher::compat_states();
    ImGui::TextColored(kMuted, "Current status");
    ImGui::SameLine(0, 12);
    ImGui::Text("%s%s", g.compatibility.c_str(), g.compat_human_set ? " (set by a person)" : " (automatic)");
    if (!g.compatibility_notes.empty()) ImGui::TextColored(kMuted, "Note: %s", g.compatibility_notes.c_str());
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextColored(kMuted, "Set the status yourself");
    ImGui::SetNextItemWidth(320);
    if (ImGui::BeginCombo("##cs", st[static_cast<size_t>(compat_idx)].c_str())) {
      for (size_t i = 0; i < st.size(); ++i) if (ImGui::Selectable(st[i].c_str(), static_cast<int>(i) == compat_idx)) compat_idx = static_cast<int>(i);
      ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(560);
    ImGui::InputTextWithHint("##cn", "Note, for example: tested 20 minutes, menu works", compat_note, sizeof compat_note);
    if (primary_button("Save status", ImVec2(160, 40), false)) {
      auto s2 = model.set_compat(g.game_id, st[static_cast<size_t>(compat_idx)], compat_note);
      compat_ok = s2.ok;
      compat_msg = s2.ok ? "Saved. This status is now yours: launches and re-imports will not change it." : "Not saved: " + s2.error.message;
    }
    note_line(compat_msg, compat_ok);
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(kMuted, "PNR only moves Untested to Launches Only after a clean exit. Playable states exist only because you set them after playing the game.");
    ImGui::PopTextWrapPos();
  }

  void tab_history(const pnr::launcher::GameEntry& g) {
    ImGui::Dummy(ImVec2(0, 6));
    auto rows = model.history(g.game_id, 50);
    if (rows.empty()) { ImGui::TextColored(kMuted, "No launches recorded yet. Press Launch to run it natively."); return; }
    if (ImGui::BeginTable("hist", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
      ImGui::TableSetupColumn("When");
      ImGui::TableSetupColumn("Result");
      ImGui::TableSetupColumn("Duration");
      ImGui::TableHeadersRow();
      for (const auto& h : rows) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(fmt_time(h.at).c_str());
        ImGui::TableSetColumnIndex(1);
        if (h.exit_code == 0) ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.5f, 1), "exit 0");
        else if (h.exit_code < 0) ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "killed by a signal");
        else ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "exit %d", h.exit_code);
        ImGui::TableSetColumnIndex(2); ImGui::Text("%.3f s", h.wall_seconds);
      }
      ImGui::EndTable();
    }
  }

  void tab_manage(const pnr::launcher::GameEntry& g) {
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::PushFont(nullptr, 20.0f); ImGui::TextUnformatted("Verify files"); ImGui::PopFont();
    ImGui::TextColored(kMuted, "Checks that the install exists and the entry point is an x86-64 ELF. Can only mark integrity failed, never verified.");
    if (ImGui::Button("Verify now", ImVec2(150, 36))) {
      auto v = model.verify(g.game_id);
      verify_ok = v.ok;
      if (v.ok) verify_msg = "Files look intact (presence and ELF header).";
      else { verify_msg = "Problems found:"; for (auto& p : v.problems) verify_msg += "\n  " + p; }
    }
    note_line(verify_msg, verify_ok);
    ImGui::Dummy(ImVec2(0, 14));
    ImGui::PushFont(nullptr, 20.0f); ImGui::TextUnformatted("Back up saves"); ImGui::PopFont();
    ImGui::TextColored(kMuted, "Writes the game's save folder to a tar archive. Refuses to overwrite an existing file.");
    ImGui::SetNextItemWidth(520);
    ImGui::InputText("##exp", export_path, sizeof export_path);
    ImGui::SameLine();
    if (ImGui::Button("Export saves", ImVec2(150, 0))) {
      auto e = model.export_saves(g.game_id, export_path);
      export_ok = e.ok;
      char b[320];
      if (e.ok) std::snprintf(b, sizeof b, "Exported %d file%s, %llu bytes.", e.files, e.files == 1 ? "" : "s", static_cast<unsigned long long>(e.bytes));
      else std::snprintf(b, sizeof b, "Not exported: %s", e.error.c_str());
      export_msg = b;
    }
    note_line(export_msg, export_ok);
    ImGui::Dummy(ImVec2(0, 14));
    ImGui::PushFont(nullptr, 20.0f); ImGui::TextUnformatted("Remove game"); ImGui::PopFont();
    ImGui::TextColored(kMuted, "Step 1 shows exactly what would be deleted. Nothing is deleted until you confirm in step 2.");
    if (ImGui::Checkbox("Keep my saves", &keep_saves)) have_dry = false;
    ImGui::SameLine(0, 20);
    if (ImGui::Button("1. Show what would be removed", ImVec2(310, 36))) { dry = model.remove(g.game_id, keep_saves, false); have_dry = true; remove_msg.clear(); }
    if (have_dry) {
      if (!dry.error.empty()) note_line("Cannot remove: " + dry.error, false);
      else {
        ImGui::Text("Install folder: %s%s", dry.install_path.c_str(), dry.install_exists ? "" : " (already missing)");
        ImGui::Text("Saves folder:   %s%s", dry.jail_path.c_str(), keep_saves ? "  (kept)" : (dry.jail_exists ? "  (will be deleted)" : " (none)"));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.22f, 0.20f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.3f, 0.27f, 1));
        if (ImGui::Button("2. Confirm: remove now", ImVec2(260, 38))) {
          auto d = model.remove(g.game_id, keep_saves, true);
          have_dry = false;
          remove_msg = d.removed ? "Removed " + g.title + "." : "Not removed: " + (d.error.empty() ? std::string("refused") : d.error);
          if (d.removed) screen = kLibrary;
        }
        ImGui::PopStyleColor(2);
      }
    }
    note_line(remove_msg, false);
  }
  void frame(float W, float H) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::Begin("root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);
    if (!sel_game() && !model.games().empty()) selected = model.games()[0].game_id;
    const float status_h = 26, rail_w = 228;
    const float body_h = H - status_h;
    rail(body_h);
    ImGui::SameLine(0, 0);
    ImGui::BeginGroup();
    const float mw = W - rail_w;
    if (screen == kSettings) {
      settings_page(mw, body_h, rail_w);
      ImGui::EndGroup();
      status_bar(W, H);
      add_popup();
      ImGui::End();
      return;
    }
    if (screen == kStore) {
      store_page(mw, body_h, rail_w);
      ImGui::EndGroup();
      status_bar(W, H);
      add_popup();
      ImGui::End();
      return;
    }
    if (screen == kGame && !sel_game()) screen = kLibrary;
    if (screen == kGame) {
      game_page(mw, body_h, rail_w);
      ImGui::EndGroup();
      status_bar(W, H);
      add_popup();
      ImGui::End();
      return;
    }
    // top bar
    ImGui::Dummy(ImVec2(mw, 22));
    ImGui::SetCursorPosX(rail_w + 36);
    ImGui::PushFont(nullptr, 30.0f);
    ImGui::TextUnformatted("Library");
    ImGui::PopFont();
    ImGui::SameLine(0, 22);
    ImGui::SetCursorPosY(24);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 18);
    ImGui::SetNextItemWidth(340);
    ImGui::InputTextWithHint("##search", "Search title, id or status", search, sizeof search);
    ImGui::PopStyleVar();
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(W - 36 - 120 - 10 - 130);
    ImGui::SetCursorPosY(24);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 18);
    if (ImGui::Button("Refresh", ImVec2(110, 0))) model.refresh();
    ImGui::SameLine(0, 12);
    if (primary_button("Add games", ImVec2(130, 34), false)) open_add = true;
    ImGui::PopStyleVar();
    ImGui::SetCursorPos(ImVec2(rail_w, 72));
    float hero_h = 252;
    hero(mw, hero_h);
    auto idx = model.filter(search);
    float cursor_y = 72 + hero_h;
    auto r = model.last_launch();
    const auto* g = sel_game();
    bool show_out = g && r.game_id == g->game_id && (r.running || r.finished);
    if (show_out) { ImGui::SetCursorPos(ImVec2(rail_w, cursor_y)); output_panel(mw); cursor_y += 118; }
    ImGui::SetCursorPos(ImVec2(rail_w, cursor_y + 14));
    if (!model.games().empty()) {
      ImGui::SetCursorPosX(rail_w + 36);
      ImGui::PushFont(nullptr, 18.0f);
      ImGui::TextColored(ImVec4(0.9f, 0.92f, 1, 1), "All games");
      ImGui::PopFont();
      ImGui::SameLine(0, 10);
      ImGui::TextColored(kMuted, "%zu", idx.size());
      cursor_y += 14 + 30;
      ImGui::SetCursorPos(ImVec2(rail_w, cursor_y));
      if (idx.empty()) { ImGui::SetCursorPosX(rail_w + 36); ImGui::TextColored(kMuted, "No game matches \"%s\".", search); }
      else grid(mw, body_h - cursor_y, idx);
    }
    ImGui::EndGroup();
    status_bar(W, H);
    add_popup();
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
    else if (a == "--open-add") o.open_add = true;
    else if (a == "--page") o.page_id = next();
    else if (a == "--store") o.open_store = true;
    else if (a == "--settings") o.settings_tab = std::atoi(next().c_str());
    else if (a == "--tab") o.tab = std::atoi(next().c_str());
    else if (a == "--scan-dir") o.scan_dir = next();
    else if (a == "--help" || a == "-h") {
      std::puts("pnr-ui [--db LIBRARY.db] [--size WxH]\n"
                "test hooks (same code paths as the mouse): --frames N --shot N:file.bmp --select ID --auto-launch ID [--auto-sandbox] --scan-dir DIR --open-add --page ID --tab N --store --settings TAB");
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
    model.store_poll();
    if (frame == 3 && o.settings_tab >= 0) { app.screen = App::kSettings; app.set_tab_want = o.settings_tab; app.plist_dirty = true; }
    if (frame == 3 && o.open_store) { app.screen = App::kStore; app.store_dirty(); }
    if (frame == 5 && !o.scan_dir.empty()) app.do_scan();
    if (frame == 3 && !o.page_id.empty()) { app.open_page(o.page_id); app.want_tab = o.tab; }
    if (frame == 8 && o.open_add) app.open_add = true;
    if (frame == 10 && !o.auto_launch.empty()) { app.selected = o.auto_launch; app.do_launch(o.auto_launch); }

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    app.frame(static_cast<float>(w), static_cast<float>(h));
    ImGui::Render();
    SDL_SetRenderDrawColor(ren, 16, 17, 22, 255);
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
