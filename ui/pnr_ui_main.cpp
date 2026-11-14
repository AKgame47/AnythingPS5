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
  bool open_add = false;
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
    const Item items[] = {{"Library", "", true}, {"Game page", "UI-2", false}, {"Store", "UI-3", false}, {"Settings", "UI-4", false},
                          {"Hardware", "planned", false}, {"Diagnostics", "planned", false}, {"Reports", "planned", false}};
    float y = 96;
    for (const Item& it : items) {
      ImGui::SetCursorPos(ImVec2(0, y));
      ImGui::InvisibleButton(it.name, ImVec2(228, 40));
      bool hov = ImGui::IsItemHovered() && it.on == false ? false : ImGui::IsItemHovered();
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
    // status bar
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, H - status_h), ImVec2(W, H), U(ImVec4(0.04f, 0.044f, 0.055f, 1)));
    char sb[512];
    std::snprintf(sb, sizeof sb, "Library %s     video %s, renderer %s     Windows: Blocked     untested on a real display/GPU", model.db_path().c_str(), video_driver.c_str(), renderer_name.c_str());
    text_at(dl, ImVec2(16, H - status_h + 5), 13, kMuted, sb, W - 32);
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
    else if (a == "--scan-dir") o.scan_dir = next();
    else if (a == "--help" || a == "-h") {
      std::puts("pnr-ui [--db LIBRARY.db] [--size WxH]\n"
                "test hooks (same code paths as the mouse): --frames N --shot N:file.bmp --select ID --auto-launch ID [--auto-sandbox] --scan-dir DIR --open-add");
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
