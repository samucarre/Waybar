#include "util/globo.hpp"

#include <gtk/gtk.h>

#include <spdlog/spdlog.h>

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
  // The root's height is its CONTENT height (GTK4), which already stops above the bar's bottom border: a popup
  // offset by this gap starts right over the border line (the bar's "line"), so the popup's own outline can carry
  // the line on around itself. Subtracting the border here again put the popups 2 px too high (3-nov-2026).
  const int h = gtk_widget_get_height(GTK_WIDGET(root)) - static_cast<int>(std::lround(b.origin.y + b.size.height));
  return h > 0 ? h : 0;
}

void Globo::enter() {
  spdlog::debug("globo: enter muted={} left={}", muted_, leftSinceMute_);
  timer_.disconnect();
  if (muted_) {
    bool back = leftSinceMute_ &&
                std::chrono::steady_clock::now() - lastLeave_ > std::chrono::milliseconds(300);
    double x = 0, y = 0;
    if (back && mutedAtKnown_ && pointerInside(&x, &y)) {
      // still where the click was (hand tremor apart): the pointer never went away
      back = std::hypot(x - mutedX_, y - mutedY_) > 8.0;
    }
    if (!back) return;
    muted_ = false;
  }
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
  spdlog::debug("globo: leave");
  timer_.disconnect();
  if (muted_) {
    leftSinceMute_ = true;
    lastLeave_ = std::chrono::steady_clock::now();
  }
  close();
}

void Globo::hide() {
  muted_ = true;
  leftSinceMute_ = false;
  mutedX_ = mutedY_ = -1000;
  mutedAtKnown_ = pointerInside(&mutedX_, &mutedY_) && mutedX_ > -999;
  close();
}

void Globo::close() {
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
    close();
  } else {
    label_.set_markup(text);
  }
}

bool Globo::pointerInside(double* x, double* y) const {
  // GTK re-sends an "enter" to the widget under the pointer's LAST position in the bar when a popup (a module menu)
  // releases its grab, although the pointer has long moved elsewhere; a tooltip shown on that stale enter would stay
  // on screen with nothing to take it away. So a tooltip only shows if the pointer really is over the widget now.
  GtkWidget* w = anchor_.gobj();
  GtkNative* native = gtk_widget_get_native(w);
  GdkSurface* surface = native != nullptr ? gtk_native_get_surface(native) : nullptr;
  GdkSeat* seat = gdk_display_get_default_seat(gtk_widget_get_display(w));
  GdkDevice* pointer = seat != nullptr ? gdk_seat_get_pointer(seat) : nullptr;
  if (surface == nullptr || pointer == nullptr) return true;  // unknown: trust the crossing events
  double sx = 0, sy = 0;
  GdkModifierType mask;
  const bool over = gdk_surface_get_device_position(surface, pointer, &sx, &sy, &mask);
  double nx = 0, ny = 0;
  gtk_native_get_surface_transform(native, &nx, &ny);
  graphene_point_t p = GRAPHENE_POINT_INIT(static_cast<float>(sx - nx), static_cast<float>(sy - ny));
  graphene_point_t out = GRAPHENE_POINT_INIT(0, 0);
  const bool computed = gtk_widget_compute_point(GTK_WIDGET(native), w, &p, &out);
  const bool inside = computed && gtk_widget_contains(w, out.x, out.y);
  spdlog::debug("globo: over={} pos=({:.1f},{:.1f}) transform=({:.1f},{:.1f}) computed={} local=({:.1f},{:.1f}) inside={}",
                over, sx, sy, nx, ny, computed, out.x, out.y, inside);
  if (!over) return false;  // not over the bar
  if (!computed) return true;
  if (inside && x != nullptr && y != nullptr) {
    *x = out.x;
    *y = out.y;
  }
  return inside;
}

void Globo::show() {
  const auto text = markup_();
  if (text.empty()) return;
  if (!pointerInside()) return;
  if (!pop_) {
    pop_ = std::make_unique<Cajon<Gtk::Popover>>("WaybarGloboPopover");
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
  pop_->abrir();
}

namespace {
const Glib::Quark& globo_key() {
  static const Glib::Quark key{"waybar-globo"};
  return key;
}
}  // namespace

Globo* find_globo(Gtk::Widget& widget) { return static_cast<Globo*>(widget.get_data(globo_key())); }

Globo& attach_globo(Gtk::Widget& widget, std::function<Glib::ustring()> markup) {
  if (auto* globo = find_globo(widget)) {
    globo->setSource(std::move(markup));
    return *globo;
  }
  auto* globo = new Globo(widget, std::move(markup));
  // Freed when the widget is finalized; the popover was unparented on its destroy (see Globo::Globo)
  widget.set_data(globo_key(), globo, [](void* data) { delete static_cast<Globo*>(data); });
  auto motion = Gtk::EventControllerMotion::create();
  motion->signal_enter().connect([globo](double, double) { globo->enter(); });
  motion->signal_leave().connect([globo] { globo->leave(); });
  widget.add_controller(motion);
  // A click answers what the tooltip was about to say: hide it, and let the click go on
  auto click = Gtk::GestureClick::create();
  click->set_button(0);
  click->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
  click->signal_pressed().connect([globo](int, double, double) { globo->hide(); });
  widget.add_controller(click);
  return *globo;
}

}  // namespace waybar::util
