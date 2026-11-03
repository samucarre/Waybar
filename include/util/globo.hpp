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
  void hide();     // hide now (click, destruction)
  void refresh();  // the text changed: update the popup if it is visible
  // Pixels between the bottom edge of `w` and the bottom edge of its window's content box (which ends above the
  // window's CSS bottom border): a popup offset by this hangs from the bar and covers the bar's border line.
  static int gap(Gtk::Widget& w);
  Gtk::Widget& anchor() const { return anchor_; }

 private:
  void show();
  bool pointerInside() const;  // is the pointer really over the anchor right now?
  Gtk::Widget& anchor_;
  std::function<Glib::ustring()> markup_;
  std::unique_ptr<Cajon<Gtk::Popover>> pop_;  // opens as an elastic drawer (util::Cajon)
  Gtk::Label label_;
  sigc::connection timer_;
  sigc::connection destroy_conn_;
  sigc::connection tooltip_conn_;
  static std::chrono::steady_clock::time_point lastHide_;
};

}  // namespace waybar::util
