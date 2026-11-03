#include "AModule.hpp"

#include <fmt/core.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <util/command.hpp>

#include "gdk/gdk.h"
#include "gdkmm/cursor.h"
#include <giomm/menu.h>
#include <giomm/simpleactiongroup.h>
#include <glibmm/markup.h>
#include <gtkmm/popovermenu.h>

#include <fstream>
#include <sstream>

namespace waybar {

AModule::AModule(const Json::Value& config, const std::string& name, const std::string& id,
                 bool enable_click, bool enable_scroll)
    : name_(name),
      config_(config),
      enableClick_{enable_click},
      enableScroll_{enable_scroll},
      isTooltip_{config_["tooltip"].isBool() ? config_["tooltip"].asBool() : true},
      isExpand_{config_["expand"].isBool() ? config_["expand"].asBool() : false},
      distance_scrolled_y_(0.0),
      distance_scrolled_x_(0.0),
      curDefault_{Gdk::Cursor::create("default")},
      curPoint_{Gdk::Cursor::create("pointer")},
      cursor_timeout_conn_() {
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

  // configure events' user commands
  // hasUserEvents is true if any element from eventMap_ is satisfying the condition in the lambda
  bool hasPressEvents{
      std::find_if(eventMap_.cbegin(), eventMap_.cend(), [&config](const auto& eventEntry) {
        // True if there is any non-release type event
        const auto& [key, action] = eventEntry;
        const auto& [button, n_press, ev_type] = key;
        return ev_type != Gdk::Event::Type::BUTTON_RELEASE && config[action].isString();
      }) != eventMap_.cend()};
  if (enable_click || hasPressEvents) {
    hasPressEvents_ = true;
  } else {
    hasPressEvents_ = false;
  }

  hasReleaseEvents_ =
      std::find_if(eventMap_.cbegin(), eventMap_.cend(), [&config](const auto& eventEntry) {
        // True if there is any release type event
        const auto& [key, action] = eventEntry;
        const auto& [button, n_press, ev_type] = key;
        return ev_type == Gdk::Event::Type::BUTTON_RELEASE && config[action].isString();
      }) != eventMap_.cend();

  makeGestureClick();
  makeControllerScroll();
  makeControllerMotion();

  // Respect user configuration of cursor
  if (config_.isMember("cursor")) {
    if (config_["cursor"].isBool() && config_["cursor"].asBool()) {
      setCursor(curPoint_);
    } else if (config_["cursor"].isString()) {
      setCursor(config_["cursor"].asString());
    } else {
      spdlog::warn("unknown cursor option configured on module {}", name_);
    }
  }
}

namespace {
// Reads a GTK3 GtkBuilder menu file (GtkMenu with GtkMenuItem / GtkSeparatorMenuItem children, as documented for
// the "menu-file" option) and turns it into a flat list of entries; a separator is an entry with empty id. A
// GtkMenuItem with <property name="sensitive">False</property> is a heading (shown, not clickable).
struct MenuEntry {
  std::string id;
  std::string label;
  bool sensitive{true};
};
class MenuFileParser : public Glib::Markup::Parser {
 public:
  std::vector<MenuEntry> entries;

 protected:
  void on_start_element(Glib::Markup::ParseContext&, const Glib::ustring& element,
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
  void on_end_element(Glib::Markup::ParseContext&, const Glib::ustring& element) override {
    if (element == "property") {
      if (inSensitive_) {
        const auto t = Glib::ustring(sensitiveText_).lowercase();
        itemSensitive_ = !(t == "false" || t == "0" || t == "no");
      }
      inLabel_ = false;
      inSensitive_ = false;
    } else if (element == "object" && inItem_) {
      inItem_ = false;
      if (!itemId_.empty()) entries.push_back({itemId_, itemLabel_, itemSensitive_});
    }
  }
  void on_text(Glib::Markup::ParseContext&, const Glib::ustring& text) override {
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

void AModule::makeMenu() {
  if (menu_ || w_ == nullptr || !config_["menu-file"].isString()) return;
  const std::string file{config_["menu-file"].asString()};
  std::ifstream in{file};
  if (!in) {
    spdlog::warn("Module {}: menu-file {} can't be read", name_, file);
    return;
  }
  std::stringstream buf;
  buf << in.rdbuf();
  MenuFileParser parser;
  try {
    Glib::Markup::ParseContext context{parser};
    context.parse(buf.str());
    context.end_parse();
  } catch (const Glib::Error& e) {
    spdlog::warn("Module {}: menu-file {} is not valid XML: {}", name_, file, std::string(e.what()));
    return;
  }
  if (parser.entries.empty()) {
    spdlog::warn("Module {}: menu-file {} has no GtkMenuItem entries", name_, file);
    return;
  }
  // A plain popover of our own (util::Cajon<Gtk::Popover>: gtkmm registers a derived type for it, so it can draw
  // itself as the elastic drawer) holding a column of flat buttons, one per entry, and separators. A
  // Gtk::PopoverMenu would look the same but gtkmm wraps the C object as a plain GtkPopoverMenu and the drawer
  // animation never gets to draw (3-nov-2026).
  menu_ = std::make_unique<util::Cajon<Gtk::Popover>>("WaybarCajonMenu");
  menu_->add_css_class("menu");  // style.css: popover.menu > contents, popover.menu button.elemento, separator
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
    const std::string id = entry.id;
    boton->signal_clicked().connect([this, id]() {
      if (menu_) menu_->popdown();
      if (config_["menu-actions"][id].isString()) {
        pid_children_.push_back(util::command::forkExec(config_["menu-actions"][id].asString()));
      } else {
        spdlog::warn("Module {}: no menu-actions entry for menu item {}", name_, id);
      }
    });
    caja->append(*boton);
  }
  menu_->set_child(*caja);
  menu_->set_parent(*w_);
  menu_->set_has_arrow(false);
  menu_->set_position(Gtk::PositionType::BOTTOM);
  // the bar window's classes (bar name, output, position) so the CSS can tell bars apart, like the globo
  if (auto* root = w_->get_root()) {
    if (auto* win = dynamic_cast<Gtk::Widget*>(root)) {
      for (const auto& c : win->get_css_classes()) menu_->add_css_class(c);
    }
  }
  // the popover has to be unparented before the module widget goes away
  w_->signal_destroy().connect([this]() {
    if (menu_) {
      menu_->unparent();
      menu_.reset();
    }
  });
}

AModule::~AModule() {
  if (cursor_timeout_conn_.connected()) {
    cursor_timeout_conn_.disconnect();
  }
  for (const auto& pid : pid_children_) {
    if (pid != -1) {
      killpg(pid, SIGTERM);
    }
  }
}

auto AModule::doUpdate() -> void {
  // Run user-provided update handler if configured
  if (config_["on-update"].isString()) {
    pid_children_.push_back(util::command::forkExec(config_["on-update"].asString()));
  }
  m_signal_updated_.emit(this);
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

void AModule::setCursor(const Glib::ustring& name) {
  this->operator Gtk::Widget&().set_cursor(name);
}

void AModule::setCursor(const Glib::RefPtr<Gdk::Cursor>& cur) {
  this->operator Gtk::Widget&().set_cursor(cur);
}

void AModule::handleMouseEnter(double x, double y) {
  this->operator Gtk::Widget&().set_state_flags(Gtk::StateFlags::PRELIGHT);
  if (globo_) globo_->enter();

  // Default behavior indicating event availability
  if (hasPressEvents_ && !config_.isMember("cursor")) {
    setCursor(curPoint_);
  }
}

void AModule::handleMouseLeave() {
  this->operator Gtk::Widget&().unset_state_flags(Gtk::StateFlags::PRELIGHT);
  if (globo_) globo_->leave();

  // Default behavior indicating event availability
  if (hasPressEvents_ && !config_.isMember("cursor")) {
    setCursor(curDefault_);
  }
}

void AModule::handlePress(int n_press, double x, double y) {
  handleClickEvent(gesture_click_->get_current_event()->get_button(), n_press, x, y,
                   Gdk::Event::Type::BUTTON_PRESS);
}

void AModule::handleRelease(int n_press, double x, double y) {
  handleClickEvent(gesture_click_->get_current_event()->get_button(), n_press, x, y,
                   Gdk::Event::Type::BUTTON_RELEASE);
}

void AModule::handleClickEvent(uint n_button, int n_press, double x, double y,
                               Gdk::Event::Type n_evtype) {
  std::string format{};
  if (globo_) globo_->hide();
  spdlog::debug("click: module {} button {} n_press {} {} at {:.0f},{:.0f}", name_, n_button, n_press,
                n_evtype == Gdk::Event::Type::BUTTON_PRESS ? "press" : "release", x, y);
  const std::map<std::tuple<uint, int, Gdk::Event::Type>, std::string>::const_iterator& rec{
      eventMap_.find(std::tuple(n_button, n_press, n_evtype))};
  if (rec != eventMap_.cend()) {
    // First call module action
    this->AModule::doAction(rec->second);
    format = rec->second;
  }
  if (rec != eventMap_.cend() && config_["menu"].isString() && rec->second == config_["menu"].asString()) {
    if (!menu_) makeMenu();
    if (menu_) {
      // hang the menu from the bar's bottom edge, not from the widget's
      menu_->set_offset(0, util::Globo::gap(*w_));
      menu_->popup();
      menu_->abrir();
    }
  }
  // Second call user scripts
  if (!format.empty()) {
    const auto actionIt{eventActionMap_.find(format)};
    const bool isModuleAction{actionIt != eventActionMap_.cend() && config_[format].isString() &&
                              config_[format].asString() == actionIt->second};

    if (isModuleAction || !config_[format].isString())
      format.clear();
    else
      format = config_[format].asString();
  }
  if (!format.empty()) {
    std::string cmd{format};
    const auto width{w_->get_width()};
    const auto height{w_->get_height()};

    if (format.find("{x}") != std::string::npos || format.find("{y}") != std::string::npos) {
      try {
        cmd = fmt::format(fmt::runtime(format), fmt::arg("x", (int)round(100. * x / width)),
                          fmt::arg("y", (int)round(100. * y / height)));
      } catch (const fmt::format_error& err) {
        spdlog::warn("Failed to format command '{}': {}. Running it unformatted.", format,
                     err.what());
        cmd = format;
      }
    }
    pid_children_.push_back(util::command::forkExec(format));
  }

  dp.emit();
}

const AModule::SCROLL_DIR AModule::getScrollDir(Glib::RefPtr<const Gdk::Event> e) {
  // only affects up/down
  bool reverse{config_["reverse-scrolling"].asBool()};
  bool reverse_mouse{config_["reverse-mouse-scrolling"].asBool()};

  // ignore reverse-scrolling if event comes from a mouse wheel
  const auto device{e->get_device()};
  if ((device) && device->get_source() == Gdk::InputSource::MOUSE) reverse = reverse_mouse;

  switch (e->get_direction()) {
    case Gdk::ScrollDirection::UP:
      return reverse ? SCROLL_DIR::DOWN : SCROLL_DIR::UP;
    case Gdk::ScrollDirection::DOWN:
      return reverse ? SCROLL_DIR::UP : SCROLL_DIR::DOWN;
    case Gdk::ScrollDirection::LEFT:
      return reverse ? SCROLL_DIR::RIGHT : SCROLL_DIR::LEFT;
    case Gdk::ScrollDirection::RIGHT:
      return reverse ? SCROLL_DIR::LEFT : SCROLL_DIR::RIGHT;
    case Gdk::ScrollDirection::SMOOTH: {
      SCROLL_DIR dir{SCROLL_DIR::NONE};

      double delta_x, delta_y;
      e->get_deltas(delta_x, delta_y);

      distance_scrolled_y_ += delta_y;
      distance_scrolled_x_ += delta_x;

      double threshold{0.0};
      if (config_["smooth-scrolling-threshold"].isNumeric()) {
        threshold = config_["smooth-scrolling-threshold"].asDouble();
      }

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
          distance_scrolled_y_ = 0.0;
          break;
        case SCROLL_DIR::LEFT:
        case SCROLL_DIR::RIGHT:
          distance_scrolled_x_ = 0.0;
          break;
        case SCROLL_DIR::NONE:
          break;
      }

      return dir;
    }
    // Silence -Wreturn-type:
    default:
      return SCROLL_DIR::NONE;
  }
}

bool AModule::handleScroll(double dx, double dy) {
  currEvent_ = controller_scroll_->get_current_event();

  if (currEvent_) {
    std::string format{};
    const auto dir{getScrollDir(currEvent_)};

    if (dir == SCROLL_DIR::UP)
      format = "on-scroll-up";
    else if (dir == SCROLL_DIR::DOWN)
      format = "on-scroll-down";
    else if (dir == SCROLL_DIR::LEFT)
      format = "on-scroll-left";
    else if (dir == SCROLL_DIR::RIGHT)
      format = "on-scroll-right";

    // First call module action
    this->AModule::doAction(format);
    // Second call user scripts
    if (config_[format].isString())
      pid_children_.push_back(util::command::forkExec(config_[format].asString()));

    dp.emit();
  }

  return true;
}

bool AModule::tooltipEnabled() const { return isTooltip_; }
bool AModule::expandEnabled() const { return isExpand_; }

void AModule::makeGestureClick() {
  if (enableClick_ || hasPressEvents_ || hasReleaseEvents_) {
    gesture_click_ = Gtk::GestureClick::create();
    gesture_click_->set_propagation_phase(Gtk::PropagationPhase::TARGET);
    gesture_click_->set_button(0u);

    if (enableClick_ || hasPressEvents_)
      gesture_click_->signal_pressed().connect(sigc::mem_fun(*this, &AModule::handlePress),
                                               isAfter_);
    if (hasReleaseEvents_)
      gesture_click_->signal_released().connect(sigc::mem_fun(*this, &AModule::handleRelease),
                                                isAfter_);
  }
}

void AModule::makeControllerScroll() {
  if (enableScroll_ || config_["on-scroll-up"].isString() || config_["on-scroll-down"].isString() ||
      config_["on-scroll-left"].isString() || config_["on-scroll-right"].isString()) {
    controller_scroll_ = Gtk::EventControllerScroll::create();
    controller_scroll_->set_propagation_phase(Gtk::PropagationPhase::TARGET);
    controller_scroll_->set_flags(Gtk::EventControllerScroll::Flags::BOTH_AXES);
    controller_scroll_->signal_scroll().connect(sigc::mem_fun(*this, &AModule::handleScroll),
                                                isAfter_);
  }
}

void AModule::makeControllerMotion() {
  controller_motion_ = Gtk::EventControllerMotion::create();
  controller_motion_->signal_enter().connect(sigc::mem_fun(*this, &AModule::handleMouseEnter));
  controller_motion_->signal_leave().connect(sigc::mem_fun(*this, &AModule::handleMouseLeave));
}

static void removeController(Glib::RefPtr<Gtk::EventController> controller) {
  if (controller) {
    Gtk::Widget* widget{controller->get_widget()};
    if (widget) widget->remove_controller(controller);
  }
}

void AModule::removeGestureClick() {
  if (gesture_click_) {
    removeController(gesture_click_);
    gesture_click_ = nullptr;
  }
}

void AModule::removeControllerScroll() {
  if (controller_scroll_) {
    removeController(controller_scroll_);
    controller_scroll_ = nullptr;
  }
}

void AModule::removeControllerMotion() {
  if (controller_motion_) {
    removeController(controller_motion_);
    controller_motion_ = nullptr;
  }
}

void AModule::bindEvents(Gtk::Widget& wg) {
  wg.set_cursor(curDefault_);

  if (!gesture_click_) makeGestureClick();
  if (!controller_scroll_) makeControllerScroll();
  if (!controller_motion_) makeControllerMotion();

  if (gesture_click_) wg.add_controller(gesture_click_);
  if (controller_scroll_) wg.add_controller(controller_scroll_);
  if (controller_motion_) wg.add_controller(controller_motion_);

  // The globo hangs from the widget the events are bound to. ALabel binds its label first and AIconLabel rebinds
  // the box afterwards (the label is empty, 0 px wide, for icon-only modules), so re-anchor when the widget changes.
  if (tooltipEnabled() && (!globo_ || &globo_->anchor() != &wg)) {
    globo_ = std::make_unique<util::Globo>(wg, [this] { return globoMarkup(); });
  }
}

Glib::ustring AModule::globoMarkup() const { return w_ != nullptr ? w_->get_tooltip_markup() : Glib::ustring(); }

void AModule::unBindEvents() {
  removeGestureClick();
  removeControllerScroll();
  removeControllerMotion();
}

}  // namespace waybar
