#include "util/globo.hpp"

#include <gtk/gtk.h>

#include <cmath>

namespace waybar::util {

std::chrono::steady_clock::time_point Globo::lastHide_{};

Globo::Globo(Gtk::Widget& anchor, std::function<Glib::ustring()> markup)
    : anchor_(anchor), markup_(std::move(markup)) {
  // GTK must not show its own tooltip: the text stays in the widget's tooltip property (modules keep calling
  // set_tooltip_markup()) and the globo renders it. set_tooltip_markup() flips has-tooltip back on, so undo it.
  anchor_.set_has_tooltip(false);
  tooltip_conn_ = anchor_.property_has_tooltip().signal_changed().connect([this] {
    if (anchor_.get_has_tooltip()) anchor_.set_has_tooltip(false);
  });
  // the popover has to be unparented before the anchor goes away
  destroy_conn_ = anchor_.signal_destroy().connect([this] {
    timer_.disconnect();
    if (pop_) {
      pop_->unparent();
      pop_.reset();
    }
  });
}

Globo::~Globo() {
  timer_.disconnect();
  destroy_conn_.disconnect();
  tooltip_conn_.disconnect();
  if (pop_) {
    pop_->unparent();
    pop_.reset();
  }
}

int Globo::gap(Gtk::Widget& w) {
  GtkRoot* root = gtk_widget_get_root(w.gobj());
  if (root == nullptr) return 0;
  graphene_rect_t b;
  if (!gtk_widget_compute_bounds(w.gobj(), GTK_WIDGET(root), &b)) return 0;
  const int h = gtk_widget_get_height(GTK_WIDGET(root)) - static_cast<int>(std::lround(b.origin.y + b.size.height));
  return h > 0 ? h : 0;
}

void Globo::enter() {
  timer_.disconnect();
  // GTK's tooltip timings: 500 ms, or 60 ms in "browse mode" right after another tooltip closed
  const bool browse = std::chrono::steady_clock::now() - lastHide_ < std::chrono::milliseconds(500);
  timer_ = Glib::signal_timeout().connect(
      [this] {
        show();
        return false;
      },
      browse ? 60 : 500);
}

void Globo::leave() {
  timer_.disconnect();
  hide();
}

void Globo::hide() {
  timer_.disconnect();
  if (pop_ && pop_->get_visible()) {
    pop_->popdown();
    lastHide_ = std::chrono::steady_clock::now();
  }
}

void Globo::refresh() {
  if (!pop_ || !pop_->get_visible()) return;
  const auto text = markup_();
  if (text.empty()) {
    hide();
  } else {
    label_.set_markup(text);
  }
}

void Globo::show() {
  const auto text = markup_();
  if (text.empty()) return;
  if (!pop_) {
    pop_ = std::make_unique<Gtk::Popover>();
    pop_->set_parent(anchor_);
    pop_->set_has_arrow(false);
    pop_->set_autohide(false);
    pop_->set_can_focus(false);
    pop_->set_can_target(false);  // read-only: never takes the pointer away from the bar
    pop_->set_position(Gtk::PositionType::BOTTOM);
    pop_->add_css_class("globo");
    // the bar window's classes (bar name, output, position) so the CSS can tell bars apart
    if (auto* root = anchor_.get_root()) {
      if (auto* win = dynamic_cast<Gtk::Widget*>(root)) {
        for (const auto& c : win->get_css_classes()) pop_->add_css_class(c);
      }
    }
    label_.set_use_markup(true);
    label_.set_xalign(0.0F);
    label_.set_justify(Gtk::Justification::LEFT);
    // natural width = the unwrapped text (GTK tooltips do the same); only very long lines wrap
    label_.set_wrap(true);
    label_.set_natural_wrap_mode(Gtk::NaturalWrapMode::NONE);
    label_.set_max_width_chars(90);
    pop_->set_child(label_);
  }
  label_.set_markup(text);
  pop_->set_offset(0, gap(anchor_));
  pop_->popup();
}

}  // namespace waybar::util
