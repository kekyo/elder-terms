#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#include <poll.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

static std::string sanitize_field(const char *value) {
  if (value == nullptr) {
    return {};
  }
  std::string result(value);
  for (char &character : result) {
    if (character == '\t' || character == '\n' || character == '\r') {
      character = ' ';
    }
  }
  return result;
}

static unsigned long window_process_id(Display *display, Window window,
                                       Atom process_id_atom) {
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long item_count = 0;
  unsigned long bytes_after = 0;
  unsigned char *data = nullptr;
  const int status =
      XGetWindowProperty(display, window, process_id_atom, 0, 1, False,
                         XA_CARDINAL, &actual_type, &actual_format, &item_count,
                         &bytes_after, &data);
  if (status != Success || actual_type != XA_CARDINAL ||
      actual_format != 32 || item_count != 1 || data == nullptr) {
    if (data != nullptr) {
      XFree(data);
    }
    return 0;
  }
  const unsigned long process_id =
      *reinterpret_cast<unsigned long *>(data);
  XFree(data);
  return process_id;
}

static bool window_has_icon(Display *display, Window window, Atom icon_atom) {
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long item_count = 0;
  unsigned long bytes_after = 0;
  unsigned char *data = nullptr;
  const int status =
      XGetWindowProperty(display, window, icon_atom, 0, 2, False, XA_CARDINAL,
                         &actual_type, &actual_format, &item_count,
                         &bytes_after, &data);
  const auto *dimensions = reinterpret_cast<unsigned long *>(data);
  const bool has_icon =
      status == Success && actual_type == XA_CARDINAL && actual_format == 32 &&
      item_count == 2 && dimensions != nullptr && dimensions[0] > 0 &&
      dimensions[1] > 0 && bytes_after > 0;
  if (data != nullptr) {
    XFree(data);
  }
  return has_icon;
}

static void report_map_event(Display *display, Window window,
                             Atom process_id_atom, Atom icon_atom) {
  char *window_name = nullptr;
  XClassHint class_hint{
      .res_name = nullptr,
      .res_class = nullptr,
  };
  (void)XFetchName(display, window, &window_name);
  (void)XGetClassHint(display, window, &class_hint);
  std::cout << "map\t" << window << '\t'
            << window_process_id(display, window, process_id_atom) << '\t'
            << sanitize_field(window_name) << '\t'
            << sanitize_field(class_hint.res_name) << '\t'
            << sanitize_field(class_hint.res_class) << '\t'
            << (window_has_icon(display, window, icon_atom) ? 1 : 0)
            << std::endl;
  if (window_name != nullptr) {
    XFree(window_name);
  }
  if (class_hint.res_name != nullptr) {
    XFree(class_hint.res_name);
  }
  if (class_hint.res_class != nullptr) {
    XFree(class_hint.res_class);
  }
}

static void drain_x11_events(Display *display, Atom process_id_atom,
                             Atom icon_atom) {
  while (XPending(display) > 0) {
    XEvent event;
    XNextEvent(display, &event);
    if (event.type == MapNotify) {
      report_map_event(display, event.xmap.window, process_id_atom, icon_atom);
    }
  }
}

static Window create_focus_competitor(Display *display, Window root) {
  const Window window =
      XCreateSimpleWindow(display, root, 80, 80, 320, 180, 1,
                          BlackPixel(display, DefaultScreen(display)),
                          WhitePixel(display, DefaultScreen(display)));
  XStoreName(display, window, "elder-terms focus competitor");
  XClassHint class_hint{
      .res_name = const_cast<char *>("elder-terms-focus-competitor"),
      .res_class = const_cast<char *>("ElderTermsFocusCompetitor"),
  };
  XSetClassHint(display, window, &class_hint);
  return window;
}

static bool click_window_without_focus(Display *display, Window window) {
  XEvent click{};
  unsigned int state = 0;
  if (!XQueryPointer(display, window, &click.xbutton.root,
                     &click.xbutton.subwindow, &click.xbutton.x_root,
                     &click.xbutton.y_root, &click.xbutton.x, &click.xbutton.y,
                     &state)) return false;
  if (click.xbutton.x < 0 || click.xbutton.y < 0) return false;

  // Use a server timestamp after the competitor gained focus. CurrentTime in
  // a synthetic input event leaves GTK's last user time unchanged and can
  // cause a subsequent focus request to be rejected as stale.
  const Window clock_window = XCreateSimpleWindow(
      display, click.xbutton.root, 0, 0, 1, 1, 0, 0, 0);
  const Atom clock_atom = XInternAtom(display, "_ELDER_TERMS_TEST_TIME", False);
  XSelectInput(display, clock_window, PropertyChangeMask);
  const unsigned char marker = 0;
  XChangeProperty(display, clock_window, clock_atom, XA_INTEGER, 8,
                   PropModeReplace, &marker, 1);
  XEvent clock_event{};
  XWindowEvent(display, clock_window, PropertyChangeMask, &clock_event);

  click.xbutton.type = ButtonPress;
  click.xbutton.display = display;
  click.xbutton.window = window;
  click.xbutton.time = clock_event.xproperty.time;
  click.xbutton.state = state;
  click.xbutton.button = Button1;
  click.xbutton.same_screen = True;
  // NoEventMask delivers to the window's creator even when input selection
  // uses XI2 instead of core masks. Bypass the WM's click-to-focus handling.
  const bool pressed = XSendEvent(display, window, False, NoEventMask, &click) != 0;
  click.xbutton.type = ButtonRelease;
  click.xbutton.state |= Button1Mask;
  const bool released = XSendEvent(display, window, False, NoEventMask, &click) != 0;
  XDestroyWindow(display, clock_window);
  XSync(display, False);
  return pressed && released;
}

static Window focused_window(Display *display) {
  Window window = None;
  int revert_to = RevertToNone;
  XGetInputFocus(display, &window, &revert_to);
  return window;
}

static bool x11_error_trapped = false;

static int on_x11_error(Display *, XErrorEvent *) {
  x11_error_trapped = true;
  return 0;
}

static bool grab_hotkey(Display *display, Window root,
                        const std::string &key_name,
                        unsigned int modifiers) {
  const KeySym keysym = XStringToKeysym(key_name.c_str());
  if (keysym == NoSymbol) {
    return false;
  }
  const KeyCode keycode = XKeysymToKeycode(display, keysym);
  if (keycode == 0) {
    return false;
  }

  constexpr unsigned int lock_masks[] = {
      0U,
      LockMask,
      Mod2Mask,
      LockMask | Mod2Mask,
  };
  x11_error_trapped = false;
  XErrorHandler previous_handler = XSetErrorHandler(on_x11_error);
  for (const unsigned int lock_mask : lock_masks) {
    XGrabKey(display, static_cast<int>(keycode), modifiers | lock_mask, root,
             True, GrabModeAsync, GrabModeAsync);
  }
  XSync(display, False);
  XSetErrorHandler(previous_handler);
  return !x11_error_trapped;
}

int main() {
  Display *display = XOpenDisplay(nullptr);
  if (display == nullptr) {
    std::cerr << "Failed to open the X11 display\n";
    return 1;
  }
  const Window root = DefaultRootWindow(display);
  XSelectInput(display, root, SubstructureNotifyMask);
  XSync(display, False);
  const Atom process_id_atom =
      XInternAtom(display, "_NET_WM_PID", False);
  const Atom icon_atom = XInternAtom(display, "_NET_WM_ICON", False);
  Window focus_competitor = None;
  std::cout << "ready" << std::endl;

  bool running = true;
  while (running) {
    pollfd descriptors[] = {
        {
            .fd = ConnectionNumber(display),
            .events = POLLIN,
            .revents = 0,
        },
        {
            .fd = STDIN_FILENO,
            .events = POLLIN,
            .revents = 0,
        },
    };
    const int result = poll(descriptors, 2, -1);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::cerr << "Failed to wait for X11 events\n";
      XCloseDisplay(display);
      return 1;
    }
    if ((descriptors[0].revents & POLLIN) != 0) {
      drain_x11_events(display, process_id_atom, icon_atom);
    }
    if ((descriptors[1].revents & (POLLIN | POLLHUP)) != 0) {
      std::string command;
      if (!std::getline(std::cin, command)) {
        running = false;
      } else if (command == "quit") {
        running = false;
      } else if (command.starts_with("barrier ")) {
        XSync(display, False);
        drain_x11_events(display, process_id_atom, icon_atom);
        std::cout << command << std::endl;
      } else if (command.starts_with("focus-competitor ")) {
        if (focus_competitor == None) {
          focus_competitor = create_focus_competitor(display, root);
        }
        XMapRaised(display, focus_competitor);
        XSetInputFocus(display, focus_competitor, RevertToParent, CurrentTime);
        XSync(display, False);
        drain_x11_events(display, process_id_atom, icon_atom);
        std::cout << command << '\t' << focus_competitor << std::endl;
      } else if (command.starts_with("click-window ")) {
        std::istringstream fields(command);
        std::string operation;
        Window window = None;
        unsigned int request_id = 0;
        fields >> operation >> std::hex >> window >> std::dec >> request_id;
        const bool sent = fields && window != None &&
            click_window_without_focus(display, window);
        drain_x11_events(display, process_id_atom, icon_atom);
        std::cout << command << '\t' << (sent ? "ok" : "failed") << std::endl;
      } else if (command.starts_with("active-window ")) {
        XSync(display, False);
        std::cout << command << '\t' << focused_window(display)
                  << std::endl;
      } else if (command.starts_with("grab-hotkey ")) {
        std::istringstream fields(command);
        std::string operation;
        std::string key_name;
        unsigned int modifiers = 0;
        unsigned int request_id = 0;
        fields >> operation >> key_name >> modifiers >> request_id;
        const bool valid = fields && operation == "grab-hotkey";
        const bool grabbed =
            valid && grab_hotkey(display, root, key_name, modifiers);
        std::cout << command << '\t' << (grabbed ? "ok" : "failed")
                  << std::endl;
      }
    }
  }

  if (focus_competitor != None) {
    XDestroyWindow(display, focus_competitor);
  }
  XCloseDisplay(display);
  return 0;
}
