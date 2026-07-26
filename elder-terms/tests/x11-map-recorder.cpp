#include <cerrno>
#include <cstdlib>
#include <iostream>
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

static void report_map_event(Display *display, Window window,
                             Atom process_id_atom) {
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
            << sanitize_field(class_hint.res_class) << std::endl;
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

static void drain_x11_events(Display *display, Atom process_id_atom) {
  while (XPending(display) > 0) {
    XEvent event;
    XNextEvent(display, &event);
    if (event.type == MapNotify) {
      report_map_event(display, event.xmap.window, process_id_atom);
    }
  }
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
      drain_x11_events(display, process_id_atom);
    }
    if ((descriptors[1].revents & (POLLIN | POLLHUP)) != 0) {
      std::string command;
      if (!std::getline(std::cin, command)) {
        running = false;
      } else if (command == "quit") {
        running = false;
      } else if (command.starts_with("barrier ")) {
        XSync(display, False);
        drain_x11_events(display, process_id_atom);
        std::cout << command << std::endl;
      }
    }
  }

  XCloseDisplay(display);
  return 0;
}
