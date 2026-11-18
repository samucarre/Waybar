#pragma once

#include <gtkmm.h>
#include <sigc++/sigc++.h>

#include "util/cajon.hpp"

#include <chrono>
#include <functional>
#include <memory>

namespace waybar::util {

// "Globo": the bar's own tooltip. GTK places its tooltip window 4 px below the widget and gives no control over
// that, so the bar cannot hang it from its bottom edge. This replaces it with a Gtk::Popover (no arrow, no focus,
// not targetable) offset so it starts exactly at the bar's bottom edge. It carries the CSS class "globo" plus the
// bar window's classes (bar name, output, position), so style.css can give it the same finish as menus and panels.
class Globo : public sigc::trackable {
 public:
  Globo(Gtk::Widget& anchor, std::function<Glib::ustring()> markup);
  ~Globo();
  void enter();    // pointer entered the widget: schedule the popup
  void leave();    // pointer left: cancel and hide
  void hide();     // hide now (click) and stay quiet until the pointer leaves the widget and comes back
  void refresh();  // the text changed: update the popup if it is visible
  // Pixels between the bottom edge of `w` and the bottom edge of its window's content box (which ends above the
  // window's CSS bottom border): a popup offset by this hangs from the bar and covers the bar's border line.
  static int gap(Gtk::Widget& w);
  Gtk::Widget& anchor() const { return anchor_; }
  // the text source (a module that also adds its own, like the clock, replaces the one ALabel gave)
  void setSource(std::function<Glib::ustring()> markup) { markup_ = std::move(markup); }

 private:
  void show();
  void close();  // pop the popup down, nothing else
  // is the pointer really over the anchor right now? (x, y): where, in the anchor's coordinates, when known
  bool pointerInside(double* x = nullptr, double* y = nullptr) const;
  Gtk::Widget& anchor_;
  std::function<Glib::ustring()> markup_;
  std::unique_ptr<Cajon<Gtk::Popover>> pop_;  // opens as an elastic drawer (util::Cajon)
  Gtk::Label label_;
  sigc::connection timer_;
  sigc::connection destroy_conn_;
  sigc::connection tooltip_conn_;
  // A click answers the question the tooltip was about to answer: after hide() no tooltip shows again over this
  // widget until the pointer has really left it and returned. GTK or the compositor re-send the crossing when the
  // click opens a panel (a leave, and an enter at once or on the next slight motion): an enter right after the leave,
  // or one that lands where the click was instead of coming in from an edge, does not count.
  bool muted_ = false;
  bool leftSinceMute_ = false;
  bool mutedAtKnown_ = false;
  double mutedX_ = 0, mutedY_ = 0;
  std::chrono::steady_clock::time_point lastLeave_{};
  static std::chrono::steady_clock::time_point lastHide_;
};

// WayBian (gtk4 port of Alexays/feat/gtk4): gives `widget` the bar's own tooltip, fed by `markup`, with the pointer
// tracking it needs (a motion controller for enter/leave and a capture-phase click that hides it, claiming nothing).
// It lives as long as the widget. Called again on the same widget, it only replaces the text source.
Globo& attach_globo(Gtk::Widget& widget, std::function<Glib::ustring()> markup);
// The globo attach_globo() gave `widget`, or nullptr.
Globo* find_globo(Gtk::Widget& widget);

}  // namespace waybar::util
