#include "AModule.hpp"

#include <fmt/core.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <util/command.hpp>
#include <variant>

#include "config.hpp"
#include "util/menu_a11y.hpp"

namespace waybar {

// Once main.cpp has reaped a child and dropped it from reap, its pid may be reused by an unrelated
// process, so it must not be signalled any more.
static void forgetReapedChildren(std::vector<int>& pids) {
  std::lock_guard<std::mutex> lock(reap_mtx);
  std::erase_if(pids,
                [](int pid) { return std::find(reap.cbegin(), reap.cend(), pid) == reap.cend(); });
}

AModule::AModule(const Json::Value& config, const std::string& name, const std::string& id,
                 bool enable_click, bool enable_scroll)
    : name_(name),
      config_(config),
      isTooltip{config_["tooltip"].isBool() ? config_["tooltip"].asBool() : true},
      isExpand{config_["expand"].isBool() ? config_["expand"].asBool() : false} {
  // Configure module action Map
  const Json::Value actions{config_["actions"]};

  disable_on_sleep_ =
      config_["disable-on-sleep"].isBool() ? config_["disable-on-sleep"].asBool() : false;

  for (Json::Value::const_iterator it = actions.begin(); it != actions.end(); ++it) {
    if (it.key().isString() && it->isString())
      if (!eventActionMap_.contains(it.key().asString())) {
        eventActionMap_.insert({it.key().asString(), it->asString()});
        enable_click = true;
        enable_scroll = true;
      } else
        spdlog::warn("Duplicate action is ignored: {0}", it.key().asString());
    else
      spdlog::warn("Wrong actions section configuration. See config by index: {}", it.index());
  }

  // hasUserEvents is true if any element from eventMap_ is satisfying the condition in the lambda
  hasUserEvents_ =
      enable_click ||
      std::any_of(eventMap_.cbegin(), eventMap_.cend(), [&config](const auto& eventEntry) {
        return eventEntry.first.second != Type::RELEASE &&
               (config[eventEntry.second].isString() || config[eventEntry.second].isBool());
      });
  hasReleaseEvent_ =
      std::any_of(eventMap_.cbegin(), eventMap_.cend(), [&config](const auto& eventEntry) {
        return eventEntry.first.second == Type::RELEASE && config[eventEntry.second].isString();
      });
  if (hasUserEvents_ || hasReleaseEvent_) {
    enableClick();
  }
  if (config_["on-scroll-up"].isString() || config_["on-scroll-down"].isString() ||
      config_["on-scroll-left"].isString() || config_["on-scroll-right"].isString() ||
      enable_scroll) {
    enableScroll();
  }

  // Respect user configuration of cursor
  if (config_.isMember("cursor")) {
    if (config_["cursor"].isBool()) {
      setCursor(config_["cursor"].asBool() ? "pointer" : "default");
    } else if (config_["cursor"].isString()) {
      setCursor(config_["cursor"].asString());
    } else if (config_["cursor"].isInt() || config_["cursor"].isUInt()) {
      // Backward-compat: legacy numeric GdkCursorType values (pre-0.16)
      static const std::map<int, const char*> legacy_cursors{
          {-2, "none"},        {34, "crosshair"}, {52, "move"}, {58, "grab"},
          {60, "pointer"},     {68, "default"},   {92, "help"}, {108, "col-resize"},
          {116, "row-resize"}, {150, "wait"},     {152, "text"}};
      auto it = legacy_cursors.find(config_["cursor"].asInt());
      setCursor(it != legacy_cursors.end() ? it->second : "default");
      spdlog::warn(
          "Numeric 'cursor' values are deprecated; use a cursor-shape-v1 name string instead "
          "(module {})",
          name_);
    } else {
      spdlog::warn("unknown cursor option configured on module {}", name_);
    }
  } else if (hasUserEvents_) {
    // Default behavior indicating event availability
    setCursor("pointer");
  }
}

void AModule::enableClick() {
  if (click_) {
    return;
  }
  click_ = Gtk::GestureClick::create();
  click_->set_button(0);
  click_->signal_pressed().connect(sigc::mem_fun(*this, &AModule::onPressed));
  click_->signal_released().connect(sigc::mem_fun(*this, &AModule::onReleased));
  event_box_.add_controller(click_);
}

void AModule::enableScroll() {
  if (scroll_) {
    return;
  }
  scroll_ = Gtk::EventControllerScroll::create();
  scroll_->set_flags(Gtk::EventControllerScroll::Flags::BOTH_AXES);
  scroll_->signal_scroll().connect(sigc::mem_fun(*this, &AModule::onScroll), false);
  event_box_.add_controller(scroll_);
}

void AModule::enableBarScroll(Gtk::Widget& bar_window) {
  enableScroll();
  if (scroll_target_ == nullptr) {
    event_box_.remove_controller(scroll_);
    bar_window.add_controller(scroll_);
    scroll_target_ = &bar_window;
  }
}

void AModule::enableChildMultiClick() {
  const bool configured =
      std::any_of(eventMap_.cbegin(), eventMap_.cend(), [this](const auto& entry) {
        const auto& name = entry.second;
        return (entry.first.second == Type::DOUBLE || entry.first.second == Type::TRIPLE) &&
               (config_[name].isString() || config_[name].isBool() ||
                eventActionMap_.contains(name) || config_["menu"] == name);
      });
  if (!configured) {
    return;
  }
  auto captured = Gtk::EventControllerLegacy::create();
  captured->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
  captured->signal_event().connect(sigc::mem_fun(*this, &AModule::onCapturedEvent), false);
  event_box_.add_controller(captured);
}

// Child gestures cancel the gestures of their ancestors when they claim a press, so the presses
// are counted by a legacy controller, which no claim cancels.
bool AModule::onCapturedEvent(const Glib::RefPtr<const Gdk::Event>& event) {
  auto* ev = const_cast<GdkEvent*>(event->gobj());
  if (gdk_event_get_event_type(ev) != GDK_BUTTON_PRESS) {
    return false;
  }
  auto* box = GTK_WIDGET(event_box_.gobj());
  auto* native = gtk_widget_get_native(box);
  double surface_x = 0;
  double surface_y = 0;
  double x = 0;
  double y = 0;
  gdk_event_get_position(ev, &x, &y);
  gtk_native_get_surface_transform(native, &surface_x, &surface_y);
  const graphene_point_t point =
      GRAPHENE_POINT_INIT(static_cast<float>(x - surface_x), static_cast<float>(y - surface_y));
  graphene_point_t local;
  if (!gtk_widget_compute_point(GTK_WIDGET(native), box, &point, &local)) {
    return false;
  }
  // A GTK3 button had its own input window, the rest of the module shared the module's one
  GtkWidget* window = gtk_widget_pick(box, local.x, local.y, GTK_PICK_DEFAULT);
  if (window != nullptr) {
    if (GtkWidget* button = gtk_widget_get_ancestor(window, GTK_TYPE_BUTTON); button != nullptr) {
      window = button;
    } else {
      window = box;
    }
  }
  int time = 0;
  int distance = 0;
  g_object_get(gtk_widget_get_settings(box), "gtk-double-click-time", &time,
               "gtk-double-click-distance", &distance, nullptr);
  const Press press{gdk_event_get_time(ev), window, gdk_button_event_get_button(ev), x, y};
  const auto follows = [&](const Press& previous, int delay) {
    return press.time < previous.time + static_cast<guint32>(delay) &&
           press.window == previous.window && press.button == previous.button &&
           std::abs(press.x - previous.x) <= distance && std::abs(press.y - previous.y) <= distance;
  };
  ButtonEvent multi{press.button, Type::PRESS, local.x, local.y};
  if (follows(presses_[1], 2 * time)) {
    multi.type = Type::TRIPLE;
    presses_ = {};
  } else {
    if (follows(presses_[0], time)) {
      multi.type = Type::DOUBLE;
    }
    presses_ = {press, presses_[0]};
  }
  if (multi.type != Type::PRESS) {
    // Dispatched once the press is handled, unless click_ sees it and reports it itself
    multi_press_ = Glib::signal_idle().connect(
        [this, multi] {
          handleToggle(multi);
          return false;
        },
        Glib::PRIORITY_HIGH);
  }
  return false;
}

void AModule::onPressed(int n_press, double x, double y) {
  multi_press_.disconnect();
  ButtonEvent ev{click_->get_current_button(), Type::PRESS, x, y};
  bool handled = handleToggle(ev);
  // GTK3 restarted its click count after a triple click
  switch ((n_press - 1) % 3) {
    case 1:
      ev.type = Type::DOUBLE;
      handled = handleToggle(ev) || handled;
      break;
    case 2:
      ev.type = Type::TRIPLE;
      handled = handleToggle(ev) || handled;
      break;
    default:
      break;
  }
  if (handled) {
    click_->set_state(Gtk::EventSequenceState::CLAIMED);
  }
}

void AModule::onReleased(int /*n_press*/, double x, double y) {
  if (hasReleaseEvent_ && handleRelease({click_->get_current_button(), Type::RELEASE, x, y})) {
    click_->set_state(Gtk::EventSequenceState::CLAIMED);
  }
}

bool AModule::onScroll(double dx, double dy) {
  // Scaled to the deltas GTK3 gave on Wayland, which smooth-scrolling-threshold is tuned for:
  // a tenth of the axis value, i.e. 1.5 per wheel detent (15 units) and surface pixels / 10.
  const bool wheel = scroll_->get_unit() == Gdk::ScrollUnit::WHEEL;
  const double factor = wheel ? 1.5 : 0.1;
  ScrollEvent ev{dx * factor, dy * factor, wheel ? GDK_SOURCE_MOUSE : GDK_SOURCE_TOUCHPAD};
  if (auto device = scroll_->get_current_event_device()) {
    ev.source = static_cast<GdkInputSource>(device->get_source());
  }
  return handleScroll(ev);
}

AModule::~AModule() {
  if (scroll_target_ != nullptr) {
    scroll_target_->remove_controller(scroll_);
  }
  forgetReapedChildren(pid_children_);
  for (const auto& pid : pid_children_) {
    if (pid != -1) {
      killpg(pid, SIGTERM);
    }
  }
  if (menu_ != nullptr) {
    menu_->unparent();
    delete menu_;
  }
}

auto AModule::update() -> void {
  // Run user-provided update handler if configured
  if (config_["on-update"].isString()) {
    forgetReapedChildren(pid_children_);
    pid_children_.push_back(util::command::forkExec(config_["on-update"].asString()));
  }
  signal_updated.emit(this);
}
// Get mapping between event name and module action name
// Then call overridden doAction in order to call appropriate module action
auto AModule::doAction(const std::string& name) -> void {
  if (!name.empty()) {
    const std::map<std::string, std::string>::const_iterator& recA{eventActionMap_.find(name)};
    // Call overridden action if derived class has implemented it
    if (recA != eventActionMap_.cend() && name != recA->second) this->doAction(recA->second);
  }
}

namespace {

// Converts a GTK3 GtkBuilder GtkMenu (the documented menu-file format) into a GMenu, since GTK4
// has no GtkMenu. Every item gets an action "menu.<id>", items without an id an inert one.
// WayBian: reads a GTK3 GtkBuilder menu file (GtkMenu with GtkMenuItem / GtkSeparatorMenuItem children, as
// documented for "menu-file") into a flat list of entries in file order; a separator is an entry with an empty
// id. A GtkMenuItem with <property name="sensitive">False</property> is a heading (shown, not clickable).
struct MenuEntry {
  std::string id;
  std::string label;
  bool sensitive{true};
};
class MenuFileParser : public Glib::Markup::Parser {
 public:
  std::vector<MenuEntry> entries;

 protected:
  void on_start_element(Glib::Markup::ParseContext& /*context*/, const Glib::ustring& element,
                        const AttributeMap& attributes) override {
    if (element == "object") {
      const auto cls = attributes.find("class");
      const auto id = attributes.find("id");
      if (cls != attributes.end() && cls->second == "GtkMenuItem") {
        inItem_ = true;
        itemId_ = id != attributes.end() ? std::string(id->second) : "";
        itemLabel_.clear();
        itemSensitive_ = true;
      } else if (cls != attributes.end() && cls->second == "GtkSeparatorMenuItem") {
        entries.push_back({"", "", true});
      }
    } else if (element == "property" && inItem_) {
      const auto name = attributes.find("name");
      inLabel_ = name != attributes.end() && name->second == "label";
      inSensitive_ = name != attributes.end() && name->second == "sensitive";
      sensitiveText_.clear();
    }
  }
  void on_end_element(Glib::Markup::ParseContext& /*context*/, const Glib::ustring& element) override {
    if (element == "property") {
      if (inSensitive_) {
        const auto v = Glib::ustring(sensitiveText_).lowercase();
        itemSensitive_ = !(v == "false" || v == "0" || v == "no");
      }
      inLabel_ = false;
      inSensitive_ = false;
    } else if (element == "object" && inItem_) {
      inItem_ = false;
      if (!itemId_.empty()) entries.push_back({itemId_, itemLabel_, itemSensitive_});
    }
  }
  void on_text(Glib::Markup::ParseContext& /*context*/, const Glib::ustring& text) override {
    if (inLabel_) itemLabel_ += text;
    if (inSensitive_) sensitiveText_ += text;
  }

 private:
  bool inItem_{false};
  bool inLabel_{false};
  bool inSensitive_{false};
  bool itemSensitive_{true};
  std::string itemId_;
  std::string itemLabel_;
  std::string sensitiveText_;
};

}  // namespace

void AModule::setupMenu() {
  if (!config_["menu"].isString()) {
    return;
  }
  try {
    std::string menuFile = config_["menu-file"].asString();
    // there might be "~" or "$HOME" in original path, try to expand it.
    auto result = Config::tryExpandPath(menuFile, "");
    if (result.empty()) {
      throw std::runtime_error("Failed to expand file: " + menuFile);
    }
    menuFile = result.front();
    std::ifstream file(menuFile);
    if (!file.is_open()) {
      throw std::runtime_error("Failed to open file: " + menuFile);
    }
    std::stringstream content;
    content << file.rdbuf();

    MenuFileParser parser;
    try {
      Glib::Markup::ParseContext context{parser};
      context.parse(content.str());
      context.end_parse();
    } catch (const Glib::Error& e) {
      throw std::runtime_error("Error found in the file " + menuFile + ": " + std::string(e.what()));
    }
    if (parser.entries.empty()) {
      throw std::runtime_error("No GtkMenuItem entries in " + menuFile);
    }
    for (auto it = config_["menu-actions"].begin(); it != config_["menu-actions"].end(); ++it) {
      const auto key = it.key().asString();
      if (std::none_of(parser.entries.begin(), parser.entries.end(),
                       [&key](const auto& entry) { return entry.id == key; })) {
        spdlog::warn("Menu item '{}' not found in builder file", key);
      }
    }

    menu_ = new util::Cajon<Gtk::Popover>("WaybarCajonMenu");
    menu_->add_css_class("menu");  // style.css: popover.menu > contents, popover.menu button.elemento
    auto* caja = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    for (const auto& entry : parser.entries) {
      if (entry.id.empty()) {
        caja->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
        continue;
      }
      auto* boton = Gtk::make_managed<Gtk::Button>();
      auto* etiqueta = Gtk::make_managed<Gtk::Label>(entry.label);
      etiqueta->set_xalign(0.0F);
      boton->set_child(*etiqueta);
      boton->add_css_class("flat");
      boton->add_css_class("elemento");
      boton->set_sensitive(entry.sensitive);
      boton->signal_clicked().connect([this, id = entry.id] {
        menu_->popdown();
        const auto& command = config_["menu-actions"][id];
        if (command.isString()) {
          waybar::util::command::forkExec(command.asString(), "GtkMenu");
        } else {
          spdlog::warn("Module {}: no menu-actions entry for menu item {}", name_, id);
        }
      });
      caja->append(*boton);
    }
    menu_->set_child(*caja);
    menu_->set_has_arrow(false);
    menu_->set_position(Gtk::PositionType::BOTTOM);
    menu_->set_parent(event_box_);
    util::unhover_menu_ancestors(GTK_WIDGET(menu_->gobj()));
  } catch (std::runtime_error& e) {
    spdlog::warn("Error while creating the menu : {}. Menu popup not activated.", e.what());
  }
}

void AModule::setCursor(const std::string& name) { event_box_.set_cursor(name); }

bool AModule::handleToggle(const ButtonEvent& e) { return handleUserEvent(e); }

bool AModule::handleRelease(const ButtonEvent& e) { return handleUserEvent(e); }

bool AModule::handleUserEvent(const ButtonEvent& e) {
  bool handled = false;
  std::string format{};
  const auto rec = eventMap_.find(std::pair(e.button, e.type));

  if (rec != eventMap_.cend()) {
    // First call module actions
    if (eventActionMap_.contains(rec->second)) {
      handled = true;
    }
    this->AModule::doAction(rec->second);

    format = rec->second;
  } else {
    // This button/event combination is not handled by this module. Return false
    // so the event can propagate (e.g. to a parent group drawer).
    dp.emit();
    return false;
  }

  // Check that a menu has been configured
  if (config_["menu"].isString()) {
    // Check if the event is the one specified for the "menu" option
    if (rec->second == config_["menu"].asString() && menu_ != nullptr) {
      // WayBian: under the module, hanging from the bar's bottom edge, with the bar window's classes
      // (bar name, output, position) so style.css can tell the bars apart, like the globo
      if (auto* root = dynamic_cast<Gtk::Widget*>(event_box_.get_root())) {
        for (const auto& cls : root->get_css_classes()) menu_->add_css_class(cls);
      }
      if (auto* globo = util::find_globo(event_box_)) globo->hide();
      menu_->set_offset(0, util::Globo::gap(event_box_));
      menu_->popup();
      menu_->abrir();
      handled = true;
    }
  }
  // Second call user scripts
  if (!format.empty()) {
    // If the configured value for this event is a recognized built-in module
    // action (registered in eventActionMap_), it has already been dispatched
    // via doAction() above / handled by the module itself. Don't additionally
    // run it as a shell command (issue #3284). Any other value is still treated
    // as a user shell command.
    const auto actionIt = eventActionMap_.find(format);
    const bool isModuleAction = actionIt != eventActionMap_.cend() && config_[format].isString() &&
                                config_[format].asString() == actionIt->second;
    if (isModuleAction || !config_[format].isString()) {
      if (isModuleAction || config_[format].isBool()) {
        handled = true;
      }
      format.clear();
    } else {
      format = config_[format].asString();
    }
  }
  if (!format.empty()) {
    handled = true;
    const int width = std::max(1, event_box_.get_width());
    const int height = std::max(1, event_box_.get_height());
    // Substitute {x}/{y} with the click position. The configured command is
    // arbitrary user input that may contain literal braces which are not {x}/{y}
    // (e.g. `echo ${HOME}`, `awk '{print $1}'`, brace expansions). Those make
    // libfmt throw fmt::format_error; since we run inside a GTK signal handler an
    // uncaught exception aborts the whole bar. Only format when a placeholder is
    // actually present, and fall back to the raw command if formatting throws.
    std::string cmd = format;
    if (format.find("{x}") != std::string::npos || format.find("{y}") != std::string::npos) {
      try {
        cmd = fmt::format(fmt::runtime(format), fmt::arg("x", (int)round(100. * e.x / width)),
                          fmt::arg("y", (int)round(100. * e.y / height)));
      } catch (const fmt::format_error& err) {
        spdlog::warn("Failed to format command '{}': {}. Running it unformatted.", format,
                     err.what());
        cmd = format;
      }
    }
    forgetReapedChildren(pid_children_);
    pid_children_.push_back(util::command::forkExec(cmd));
  }
  dp.emit();
  return handled;
}

AModule::SCROLL_DIR AModule::getScrollDir(const ScrollEvent& e) {
  // only affects up/down
  bool reverse = config_["reverse-scrolling"].asBool();
  // ignore reverse-scrolling if event comes from a mouse wheel
  if (e.source == GDK_SOURCE_MOUSE) {
    reverse = config_["reverse-mouse-scrolling"].asBool();
  }

  distance_scrolled_y_ += e.dy;
  distance_scrolled_x_ += e.dx;

  double threshold = 0;
  if (config_["smooth-scrolling-threshold"].isNumeric()) {
    threshold = config_["smooth-scrolling-threshold"].asDouble();
  }

  SCROLL_DIR dir{SCROLL_DIR::NONE};
  if (distance_scrolled_y_ < -threshold) {
    dir = reverse ? SCROLL_DIR::DOWN : SCROLL_DIR::UP;
  } else if (distance_scrolled_y_ > threshold) {
    dir = reverse ? SCROLL_DIR::UP : SCROLL_DIR::DOWN;
  } else if (distance_scrolled_x_ > threshold) {
    dir = SCROLL_DIR::RIGHT;
  } else if (distance_scrolled_x_ < -threshold) {
    dir = SCROLL_DIR::LEFT;
  }

  switch (dir) {
    case SCROLL_DIR::UP:
    case SCROLL_DIR::DOWN:
      distance_scrolled_y_ = 0;
      break;
    case SCROLL_DIR::LEFT:
    case SCROLL_DIR::RIGHT:
      distance_scrolled_x_ = 0;
      break;
    case SCROLL_DIR::NONE:
      break;
  }
  return dir;
}

bool AModule::handleScroll(const ScrollEvent& e) {
  auto dir = getScrollDir(e);
  std::string eventName{};

  if (dir == SCROLL_DIR::UP)
    eventName = "on-scroll-up";
  else if (dir == SCROLL_DIR::DOWN)
    eventName = "on-scroll-down";
  else if (dir == SCROLL_DIR::LEFT)
    eventName = "on-scroll-left";
  else if (dir == SCROLL_DIR::RIGHT)
    eventName = "on-scroll-right";

  // First call module actions
  this->AModule::doAction(eventName);
  // Second call user scripts
  if (config_[eventName].isString()) {
    forgetReapedChildren(pid_children_);
    pid_children_.push_back(util::command::forkExec(config_[eventName].asString()));
  }

  dp.emit();
  return true;
}

bool AModule::tooltipEnabled() const { return isTooltip; }
bool AModule::expandEnabled() const { return isExpand; }

AModule::operator Gtk::Widget&() { return event_box_; }

}  // namespace waybar
