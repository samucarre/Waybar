#pragma once

#include <gtk/gtk.h>
#include <gtkmm.h>

#include <spdlog/spdlog.h>

#include <cmath>
#include <utility>

namespace waybar::util {

// "Cajón": the elastic drawer the bar's own panels open with (perfilado.py, 3-nov-2026, the "suave" bounce Samuel
// chose). The popup's picture is scaled vertically from its TOP edge, which stays stuck to the bar: scaleY goes
// 0.12 → 1.05 → 1 over 700 ms, cubic-bezier(.33,.9,.35,1) on each leg. Only the drawing changes; the popup surface
// keeps its full size, so the layout and the pointer are untouched. Closing is immediate.
// Base must be a class gtkmm builds through Glib::ConstructParams (Gtk::Popover does; Gtk::PopoverMenu does NOT: it
// wraps a C-created object and keeps the plain GtkPopoverMenu type, so no vfunc is ever hooked); the custom GType
// name makes gtkmm register a derived type whose class calls snapshot_vfunc.
template <class Base>
class Cajon : public Base {
 public:
  template <class... Args>
  explicit Cajon(const char* tipo, Args&&... args) : Glib::ObjectBase(tipo), Base(std::forward<Args>(args)...) {}
  ~Cajon() override {
    if (tick_ != 0) this->remove_tick_callback(tick_);
  }

  // Start the opening animation; call right after popup(). The ticks run once the popup is mapped.
  void abrir() {
    if (!animaciones()) {
      escala_ = 1.0;
      return;
    }
    escala_ = 0.12;
    t0_ = 0;
    tPrev_ = 0;
    spdlog::debug("cajon: abrir ({})", G_OBJECT_TYPE_NAME(this->gobj()));
    if (tick_ != 0) this->remove_tick_callback(tick_);
    tick_ = this->add_tick_callback(
        [this](const Glib::RefPtr<Gdk::FrameClock>& clock) { return anima(clock); });
    this->queue_draw();
  }

 protected:
  void snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) override {
    if (escala_ >= 1.0) {
      Base::snapshot_vfunc(snapshot);
      return;
    }
    snapshot->save();
    gtk_snapshot_scale(snapshot->gobj(), 1.0F, static_cast<float>(escala_ > 0 ? escala_ : 0.0001));
    Base::snapshot_vfunc(snapshot);
    snapshot->restore();
  }

 private:
  static constexpr double DURACION = 0.7;  // s

  static bool animaciones() {
    auto settings = Gtk::Settings::get_default();
    return settings ? settings->property_gtk_enable_animations().get_value() : true;
  }

  // y of a CSS cubic-bezier (p1=(x1,y1), p2=(x2,y2)) for the abscissa x in [0,1]
  static double bezier(double x, double x1, double y1, double x2, double y2) {
    if (x <= 0) return 0.0;
    if (x >= 1) return 1.0;
    auto cx = [&](double t) { return 3 * (1 - t) * (1 - t) * t * x1 + 3 * (1 - t) * t * t * x2 + t * t * t; };
    auto cy = [&](double t) { return 3 * (1 - t) * (1 - t) * t * y1 + 3 * (1 - t) * t * t * y2 + t * t * t; };
    double lo = 0.0, hi = 1.0, t = x;
    for (int i = 0; i < 24; ++i) {
      const double xe = cx(t);
      if (std::fabs(xe - x) < 1e-4) break;
      if (xe < x) lo = t; else hi = t;
      t = (lo + hi) / 2;
    }
    return cy(t);
  }

  // scaleY 0.12 → 1.05 → 1, the two legs of the "suave" bounce (p = progress 0..1)
  static double escalaSuave(double p) {
    if (p >= 1) return 1.0;
    if (p < 0.6) return 0.12 + (1.05 - 0.12) * bezier(p / 0.6, .33, .9, .35, 1);
    return 1.05 + (1.0 - 1.05) * bezier((p - 0.6) / 0.4, .33, .9, .35, 1);
  }

  bool anima(const Glib::RefPtr<Gdk::FrameClock>& clock) {
    const gint64 t = clock->get_frame_time();
    if (t0_ == 0) {
      t0_ = t;
      tPrev_ = t;
    }
    // A main-loop stall (> 80 ms between ticks) shifts the origin: the drawer carries on from where it was instead
    // of jumping to the end with no frame in between (same rule as the bar's panels and the notification center).
    if (t - tPrev_ > 80000) t0_ += (t - tPrev_) - 16000;
    tPrev_ = t;
    const double p = static_cast<double>(t - t0_) / 1e6 / DURACION;
    escala_ = escalaSuave(p);
    this->queue_draw();
    if (p >= 1.0) {
      tick_ = 0;
      escala_ = 1.0;
      return false;
    }
    return true;
  }

  double escala_ = 1.0;
  gint64 t0_ = 0;
  gint64 tPrev_ = 0;
  guint tick_ = 0;
};

}  // namespace waybar::util
