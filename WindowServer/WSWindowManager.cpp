#include "WSWindowManager.h"
#include "WSWindow.h"
#include "WSScreen.h"
#include "WSMessageLoop.h"
#include <SharedGraphics/Font.h>
#include <SharedGraphics/Painter.h>
#include <SharedGraphics/CharacterBitmap.h>
#include <AK/StdLibExtras.h>
#include <LibC/errno_numbers.h>
#include "WSMenu.h"
#include "WSMenuBar.h"
#include "WSMenuItem.h"
#include <WindowServer/WSClientConnection.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>

#ifdef KERNEL
#include <Kernel/ProcFS.h>
#endif

//#define DEBUG_COUNTERS
//#define DEBUG_WID_IN_TITLE_BAR
#define RESIZE_DEBUG
#define USE_WALLPAPER

static const int window_titlebar_height = 16;

static inline Rect menu_window_rect(const Rect& rect)
{
    return rect.inflated(2, 2);
}

static inline Rect title_bar_rect(const Rect& window)
{
    return {
        window.x() - 1,
        window.y() - window_titlebar_height,
        window.width() + 2,
        window_titlebar_height
    };
}

static inline Rect title_bar_text_rect(const Rect& window)
{
    auto titlebar_rect = title_bar_rect(window);
    return {
        titlebar_rect.x() + 2,
        titlebar_rect.y(),
        titlebar_rect.width() - 4,
        titlebar_rect.height()
    };
}

static inline Rect close_button_rect_for_window(const Rect& window_rect)
{
    auto titlebar_inner_rect = title_bar_text_rect(window_rect);
    int close_button_margin = 1;
    int close_button_size = titlebar_inner_rect.height() - close_button_margin * 2;
    return Rect {
        titlebar_inner_rect.right() - close_button_size + 1,
        titlebar_inner_rect.top() + close_button_margin,
        close_button_size,
        close_button_size - 1
    };
}

static inline Rect border_window_rect(const Rect& window)
{
    auto titlebar_rect = title_bar_rect(window);
    return { titlebar_rect.x() - 1,
        titlebar_rect.y() - 1,
        titlebar_rect.width() + 2,
        window_titlebar_height + window.height() + 3
    };
}

static inline Rect outer_window_rect(const Rect& window)
{
    auto rect = border_window_rect(window);
    rect.inflate(2, 2);
    return rect;
}

static WSWindowManager* s_the;

WSWindowManager& WSWindowManager::the()
{
    ASSERT(s_the);
    return *s_the;
}

static const char* cursor_bitmap_inner_ascii = {
    " #          "
    " ##         "
    " ###        "
    " ####       "
    " #####      "
    " ######     "
    " #######    "
    " ########   "
    " #########  "
    " ########## "
    " ######     "
    " ##  ##     "
    " #    ##    "
    "      ##    "
    "       ##   "
    "       ##   "
    "            "
};

static const char* cursor_bitmap_outer_ascii = {
    "##          "
    "# #         "
    "#  #        "
    "#   #       "
    "#    #      "
    "#     #     "
    "#      #    "
    "#       #   "
    "#        #  "
    "#         # "
    "#      #### "
    "#  ##  #    "
    "# #  #  #   "
    "##   #  #   "
    "      #  #  "
    "      #  #  "
    "       ##   "
};

void WSWindowManager::flip_buffers()
{
    swap(m_front_bitmap, m_back_bitmap);
    swap(m_front_painter, m_back_painter);
    int new_y_offset = m_buffers_are_flipped ? 0 : m_screen_rect.height();
    WSScreen::the().set_y_offset(new_y_offset);
    m_buffers_are_flipped = !m_buffers_are_flipped;
}

WSWindowManager::WSWindowManager()
    : m_screen(WSScreen::the())
    , m_screen_rect(m_screen.rect())
    , m_flash_flush(false)
{
    s_the = this;

#ifndef DEBUG_COUNTERS
    (void)m_compose_count;
    (void)m_flush_count;
#endif
    auto size = m_screen_rect.size();
    m_front_bitmap = GraphicsBitmap::create_wrapper(GraphicsBitmap::Format::RGB32, size, m_screen.scanline(0));
    m_back_bitmap = GraphicsBitmap::create_wrapper(GraphicsBitmap::Format::RGB32, size, m_screen.scanline(size.height()));

    m_front_painter = make<Painter>(*m_front_bitmap);
    m_back_painter = make<Painter>(*m_back_bitmap);

    m_font = Font::default_font();

    m_front_painter->set_font(font());
    m_back_painter->set_font(font());

    m_background_color = Color(50, 50, 50);
    m_active_window_border_color = Color(110, 34, 9);
    m_active_window_border_color2 = Color(244, 202, 158);
    m_active_window_title_color = Color::White;
    m_inactive_window_border_color = Color(128, 128, 128);
    m_inactive_window_border_color2 = Color(192, 192, 192);
    m_inactive_window_title_color = Color(213, 208, 199);
    m_dragging_window_border_color = Color(161, 50, 13);
    m_dragging_window_border_color2 = Color(250, 220, 187);
    m_dragging_window_title_color = Color::White;

    m_cursor_bitmap_inner = CharacterBitmap::create_from_ascii(cursor_bitmap_inner_ascii, 12, 17);
    m_cursor_bitmap_outer = CharacterBitmap::create_from_ascii(cursor_bitmap_outer_ascii, 12, 17);

#ifdef USE_WALLPAPER
    m_wallpaper_path = "/res/wallpapers/cool.rgb";
    m_wallpaper = GraphicsBitmap::load_from_file(GraphicsBitmap::Format::RGBA32, m_wallpaper_path, { 1024, 768 });
#endif

#ifdef KERNEL
    ProcFS::the().add_sys_bool("wm_flash_flush", m_flash_flush);
    ProcFS::the().add_sys_string("wm_wallpaper", m_wallpaper_path, [this] {
        m_wallpaper = GraphicsBitmap::load_from_file(GraphicsBitmap::Format::RGBA32, m_wallpaper_path, m_screen_rect.size());
        invalidate(m_screen_rect);
    });
#endif

    m_menu_selection_color = Color::from_rgb(0x84351a);

    {
        byte system_menu_name[] = { 0xf8, 0 };
        m_system_menu = make<WSMenu>(nullptr, -1, String((const char*)system_menu_name));
        m_system_menu->add_item(make<WSMenuItem>(0, "Launch Terminal"));
        m_system_menu->add_item(make<WSMenuItem>(WSMenuItem::Separator));
        m_system_menu->add_item(make<WSMenuItem>(1, "640x480"));
        m_system_menu->add_item(make<WSMenuItem>(2, "800x600"));
        m_system_menu->add_item(make<WSMenuItem>(3, "1024x768"));
        m_system_menu->add_item(make<WSMenuItem>(WSMenuItem::Separator));
        m_system_menu->add_item(make<WSMenuItem>(4, "About..."));
        m_system_menu->on_item_activation = [this] (WSMenuItem& item) {
            if (item.identifier() == 0) {
                if (fork() == 0) {
                    execl("/bin/Terminal", "/bin/Terminal", nullptr);
                    ASSERT_NOT_REACHED();
                }
                return;
            }
            switch (item.identifier()) {
            case 1: set_resolution(640, 480); break;
            case 2: set_resolution(800, 600); break;
            case 3: set_resolution(1024, 768); break;
            }
            if (item.identifier() == 4) {
                if (fork() == 0) {
                    execl("/bin/About", "/bin/About", nullptr);
                    ASSERT_NOT_REACHED();
                }
                return;
            }
            dbgprintf("WSMenu 1 item activated: '%s'\n", item.text().characters());
        };
    }

    // NOTE: This ensures that the system menu has the correct dimensions.
    set_current_menubar(nullptr);

    WSMessageLoop::the().start_timer(300, [this] {
        static time_t last_update_time;
        time_t now = time(nullptr);
        if (now != last_update_time) {
            invalidate(menubar_rect());
            last_update_time = now;
        }
    });

    invalidate();
    compose();
}

WSWindowManager::~WSWindowManager()
{
}

void WSWindowManager::set_resolution(int width, int height)
{
    if (m_screen_rect.width() == width && m_screen_rect.height() == height)
        return;
    m_screen.set_resolution(width, height);
    m_screen_rect = m_screen.rect();
    m_front_bitmap = GraphicsBitmap::create_wrapper(GraphicsBitmap::Format::RGB32, { width, height }, m_screen.scanline(0));
    m_back_bitmap = GraphicsBitmap::create_wrapper(GraphicsBitmap::Format::RGB32, { width, height }, m_screen.scanline(height));
    m_front_painter = make<Painter>(*m_front_bitmap);
    m_back_painter = make<Painter>(*m_back_bitmap);
    m_buffers_are_flipped = false;
    invalidate();
    compose();
}

template<typename Callback>
void WSWindowManager::for_each_active_menubar_menu(Callback callback)
{
    callback(*m_system_menu);
    if (m_current_menubar)
        m_current_menubar->for_each_menu(callback);
}

int WSWindowManager::menubar_menu_margin() const
{
    return 16;
}

void WSWindowManager::set_current_menubar(WSMenuBar* menubar)
{
    if (menubar)
        m_current_menubar = menubar->make_weak_ptr();
    else
        m_current_menubar = nullptr;
    dbgprintf("[WM] Current menubar is now %p\n", menubar);
    Point next_menu_location { menubar_menu_margin() / 2, 3 };
    for_each_active_menubar_menu([&] (WSMenu& menu) {
        int text_width = font().width(menu.name());
        menu.set_rect_in_menubar({ next_menu_location.x() - menubar_menu_margin() / 2, 0, text_width + menubar_menu_margin(), menubar_rect().height() - 1 });
        menu.set_text_rect_in_menubar({ next_menu_location, { text_width, font().glyph_height() } });
        next_menu_location.move_by(menu.rect_in_menubar().width(), 0);
        return true;
    });
    invalidate(menubar_rect());
}

static const char* s_close_button_bitmap_data = {
    "##    ##"
    "###  ###"
    " ###### "
    "  ####  "
    "   ##   "
    "  ####  "
    " ###### "
    "###  ###"
    "##    ##"
};

static CharacterBitmap* s_close_button_bitmap;
static const int s_close_button_bitmap_width = 8;
static const int s_close_button_bitmap_height = 9;

void WSWindowManager::paint_window_frame(WSWindow& window)
{
    //printf("[WM] paint_window_frame {%p}, rect: %d,%d %dx%d\n", &window, window.rect().x(), window.rect().y(), window.rect().width(), window.rect().height());

    if (window.type() == WSWindowType::Menu) {
        m_back_painter->draw_rect(menu_window_rect(window.rect()), Color::LightGray);
        return;
    }

    auto titlebar_rect = title_bar_rect(window.rect());
    auto titlebar_inner_rect = title_bar_text_rect(window.rect());
    auto outer_rect = outer_window_rect(window.rect());
    auto border_rect = border_window_rect(window.rect());
    auto close_button_rect = close_button_rect_for_window(window.rect());

    auto titlebar_title_rect = titlebar_inner_rect;
    titlebar_title_rect.set_width(font().glyph_width() * window.title().length());

    Rect inner_border_rect {
        window.x() - 1,
        window.y() - 1,
        window.width() + 2,
        window.height() + 2
    };

    Color title_color;
    Color border_color;
    Color border_color2;
    Color middle_border_color;

    if (&window == m_drag_window.ptr()) {
        border_color = m_dragging_window_border_color;
        border_color2 = m_dragging_window_border_color2;
        title_color = m_dragging_window_title_color;
        middle_border_color = Color::White;
    } else if (&window == m_active_window.ptr()) {
        border_color = m_active_window_border_color;
        border_color2 = m_active_window_border_color2;
        title_color = m_active_window_title_color;
        middle_border_color = Color::MidGray;
    } else {
        border_color = m_inactive_window_border_color;
        border_color2 = m_inactive_window_border_color2;
        title_color = m_inactive_window_title_color;
        middle_border_color = Color::MidGray;
    }

    m_back_painter->fill_rect_with_gradient(titlebar_rect, border_color, border_color2);
    for (int i = 2; i <= titlebar_inner_rect.height() - 4; i += 2) {
        m_back_painter->draw_line({ titlebar_title_rect.right() + 4, titlebar_inner_rect.y() + i }, { close_button_rect.left() - 3, titlebar_inner_rect.y() + i }, border_color);
    }
    m_back_painter->draw_rect(border_rect, middle_border_color);
    m_back_painter->draw_rect(outer_rect, border_color);
    m_back_painter->draw_rect(inner_border_rect, border_color);
    m_back_painter->draw_text(titlebar_title_rect, window.title(), TextAlignment::CenterLeft, title_color);

    if (!s_close_button_bitmap)
        s_close_button_bitmap = CharacterBitmap::create_from_ascii(s_close_button_bitmap_data, s_close_button_bitmap_width, s_close_button_bitmap_height).leak_ref();

    m_back_painter->fill_rect_with_gradient(close_button_rect.shrunken(2, 2), Color::LightGray, Color::White);

    m_back_painter->draw_rect(close_button_rect, Color::DarkGray, true);
    auto x_location = close_button_rect.center();
    x_location.move_by(-(s_close_button_bitmap_width / 2), -(s_close_button_bitmap_height / 2));
    m_back_painter->draw_bitmap(x_location, *s_close_button_bitmap, Color::Black);

#ifdef DEBUG_WID_IN_TITLE_BAR
    Color metadata_color(96, 96, 96);
    m_back_painter->draw_text(
        titlebar_inner_rect,
        String::format("%d:%d", window.pid(), window.window_id()),
        TextAlignment::CenterRight,
        metadata_color
    );
#endif
}

void WSWindowManager::add_window(WSWindow& window)
{
    m_windows.set(&window);
    m_windows_in_order.append(&window);
    if (!active_window())
        set_active_window(&window);
}

void WSWindowManager::move_to_front(WSWindow& window)
{
    if (m_windows_in_order.tail() != &window)
        invalidate(window);
    m_windows_in_order.remove(&window);
    m_windows_in_order.append(&window);
}

void WSWindowManager::remove_window(WSWindow& window)
{
    if (!m_windows.contains(&window))
        return;

    invalidate(window);
    m_windows.remove(&window);
    m_windows_in_order.remove(&window);
    if (!active_window() && !m_windows.is_empty())
        set_active_window(*m_windows.begin());
}

void WSWindowManager::notify_title_changed(WSWindow& window)
{
    printf("[WM] WSWindow{%p} title set to '%s'\n", &window, window.title().characters());
    invalidate(outer_window_rect(window.rect()));
}

void WSWindowManager::notify_rect_changed(WSWindow& window, const Rect& old_rect, const Rect& new_rect)
{
    printf("[WM] WSWindow %p rect changed (%d,%d %dx%d) -> (%d,%d %dx%d)\n", &window, old_rect.x(), old_rect.y(), old_rect.width(), old_rect.height(), new_rect.x(), new_rect.y(), new_rect.width(), new_rect.height());
    invalidate(outer_window_rect(old_rect));
    invalidate(outer_window_rect(new_rect));
}

void WSWindowManager::handle_menu_mouse_event(WSMenu& menu, WSMouseEvent& event)
{
    bool is_hover_with_any_menu_open = event.type() == WSMouseEvent::MouseMove && m_current_menu;
    bool is_mousedown_with_left_button = event.type() == WSMouseEvent::MouseDown && event.button() == MouseButton::Left;
    bool should_open_menu = &menu != current_menu() && (is_hover_with_any_menu_open || is_mousedown_with_left_button);

    if (should_open_menu) {
        if (current_menu() == &menu)
            return;
        close_current_menu();
        if (!menu.is_empty()) {
            auto& menu_window = menu.ensure_menu_window();
            menu_window.move_to({ menu.rect_in_menubar().x(), menu.rect_in_menubar().bottom() });
            menu_window.set_visible(true);
        }
        m_current_menu = menu.make_weak_ptr();
        return;
    }
    if (event.type() == WSMouseEvent::MouseDown && event.button() == MouseButton::Left) {
        close_current_menu();
        return;
    }
}

void WSWindowManager::close_current_menu()
{
    if (m_current_menu && m_current_menu->menu_window())
        m_current_menu->menu_window()->set_visible(false);
    m_current_menu = nullptr;
}

void WSWindowManager::handle_menubar_mouse_event(WSMouseEvent& event)
{
    for_each_active_menubar_menu([&] (WSMenu& menu) {
        if (menu.rect_in_menubar().contains(event.position())) {
            handle_menu_mouse_event(menu, event);
            return false;
        }
        return true;
    });
}

void WSWindowManager::handle_titlebar_mouse_event(WSWindow& window, WSMouseEvent& event)
{
    if (event.type() == WSMessage::MouseDown && event.button() == MouseButton::Left) {
#ifdef DRAG_DEBUG
        printf("[WM] Begin dragging WSWindow{%p}\n", &window);
#endif
        m_drag_window = window.make_weak_ptr();;
        m_drag_origin = event.position();
        m_drag_window_origin = window.position();
        invalidate(window);
        return;
    }
}

void WSWindowManager::handle_close_button_mouse_event(WSWindow& window, WSMouseEvent& event)
{
    if (event.type() == WSMessage::MouseDown && event.button() == MouseButton::Left) {
        WSMessage message(WSMessage::WindowCloseRequest);
        window.on_message(message);
        return;
    }
}

void WSWindowManager::start_window_resize(WSWindow& window, WSMouseEvent& event)
{
#ifdef RESIZE_DEBUG
    printf("[WM] Begin resizing WSWindow{%p}\n", &window);
#endif
    m_resize_window = window.make_weak_ptr();;
    m_resize_origin = event.position();
    m_resize_window_original_rect = window.rect();
    m_resize_window->set_has_painted_since_last_resize(true);
    invalidate(window);
}

void WSWindowManager::process_mouse_event(WSMouseEvent& event, WSWindow*& event_window)
{
    event_window = nullptr;

    if (m_drag_window) {
        if (event.type() == WSMessage::MouseUp && event.button() == MouseButton::Left) {
#ifdef DRAG_DEBUG
            printf("[WM] Finish dragging WSWindow{%p}\n", m_drag_window.ptr());
#endif
            invalidate(*m_drag_window);
            m_drag_window = nullptr;
            return;
        }

        if (event.type() == WSMessage::MouseMove) {
            auto old_window_rect = m_drag_window->rect();
            Point pos = m_drag_window_origin;
#ifdef DRAG_DEBUG
            dbgprintf("[WM] Dragging [origin: %d,%d] now: %d,%d\n", m_drag_origin.x(), m_drag_origin.y(), event.x(), event.y());
#endif
            pos.move_by(event.x() - m_drag_origin.x(), event.y() - m_drag_origin.y());
            m_drag_window->set_position_without_repaint(pos);
            invalidate(outer_window_rect(old_window_rect));
            invalidate(outer_window_rect(m_drag_window->rect()));
            return;
        }
    }

    if (m_resize_window) {
        if (event.type() == WSMessage::MouseUp && event.button() == MouseButton::Right) {
#ifdef RESIZE_DEBUG
            printf("[WM] Finish resizing WSWindow{%p}\n", m_resize_window.ptr());
#endif
            WSMessageLoop::the().post_message(m_resize_window.ptr(), make<WSResizeEvent>(m_resize_window->rect(), m_resize_window->rect()));
            invalidate(*m_resize_window);
            m_resize_window = nullptr;
            return;
        }

        if (event.type() == WSMessage::MouseMove) {
            auto old_rect = m_resize_window->rect();
            int dx = event.x() - m_resize_origin.x();
            int dy = event.y() - m_resize_origin.y();
            auto new_rect = m_resize_window_original_rect;
            new_rect.set_width(max(50, new_rect.width() + dx));
            new_rect.set_height(max(50, new_rect.height() + dy));

            if (!m_resize_window->size_increment().is_null()) {
                int horizontal_incs = (new_rect.width() - m_resize_window->base_size().width()) / m_resize_window->size_increment().width();
                new_rect.set_width(m_resize_window->base_size().width() + horizontal_incs * m_resize_window->size_increment().width());
                int vertical_incs = (new_rect.height() - m_resize_window->base_size().height()) / m_resize_window->size_increment().height();
                new_rect.set_height(m_resize_window->base_size().height() + vertical_incs * m_resize_window->size_increment().height());
            }

            if (m_resize_window->rect() == new_rect)
                return;
#ifdef RESIZE_DEBUG
            dbgprintf("[WM] Resizing [original: %s] now: %s\n",
                m_resize_window_original_rect.to_string().characters(),
                new_rect.to_string().characters());
#endif
            m_resize_window->set_rect(new_rect);
            if (m_resize_window->has_painted_since_last_resize()) {
                m_resize_window->set_has_painted_since_last_resize(false);
                dbgprintf("I'm gonna wait for %s\n", new_rect.to_string().characters());
                m_resize_window->set_last_lazy_resize_rect(new_rect);
                WSMessageLoop::the().post_message(m_resize_window.ptr(), make<WSResizeEvent>(old_rect, new_rect));
            }
            return;
        }
    }

    for (auto* window = m_windows_in_order.tail(); window; window = window->prev()) {
        if (!window->global_cursor_tracking())
            continue;
        ASSERT(window->is_visible()); // Maybe this should be supported? Idk. Let's catch it and think about it later.
        Point position { event.x() - window->rect().x(), event.y() - window->rect().y() };
        auto local_event = make<WSMouseEvent>(event.type(), position, event.buttons(), event.button());
        window->on_message(*local_event);
    }

    if (menubar_rect().contains(event.position())) {
        handle_menubar_mouse_event(event);
        return;
    }

    if (m_current_menu && m_current_menu->menu_window()) {
        bool event_is_inside_current_menu = m_current_menu->menu_window()->rect().contains(event.position());
        if (!event_is_inside_current_menu) {
            if (m_current_menu->hovered_item())
                m_current_menu->clear_hovered_item();
            if (event.type() == WSMessage::MouseDown || event.type() == WSMessage::MouseUp)
                close_current_menu();
        }
    }

    for_each_visible_window_from_front_to_back([&] (WSWindow& window) {
        if (window.type() != WSWindowType::Menu && title_bar_rect(window.rect()).contains(event.position())) {
            if (event.type() == WSMessage::MouseDown) {
                move_to_front(window);
                set_active_window(&window);
            }
            if (close_button_rect_for_window(window.rect()).contains(event.position())) {
                handle_close_button_mouse_event(window, event);
                return IterationDecision::Abort;
            }
            handle_titlebar_mouse_event(window, event);
            return IterationDecision::Abort;
        }

        if (window.rect().contains(event.position())) {
            if (window.type() != WSWindowType::Menu && event.type() == WSMessage::MouseDown) {
                move_to_front(window);
                set_active_window(&window);
            }
            if (event.type() == WSMessage::MouseDown && event.button() == MouseButton::Right) {
                start_window_resize(window, event);
                return IterationDecision::Abort;
            }
            event_window = &window;
            // FIXME: Should we just alter the coordinates of the existing MouseEvent and pass it through?
            Point position { event.x() - window.rect().x(), event.y() - window.rect().y() };
            auto local_event = make<WSMouseEvent>(event.type(), position, event.buttons(), event.button());
            window.on_message(*local_event);
            return IterationDecision::Abort;
        }
        return IterationDecision::Continue;
    });
}

template<typename Callback>
IterationDecision WSWindowManager::for_each_visible_window_of_type_from_back_to_front(WSWindowType type, Callback callback)
{
    for (auto* window = m_windows_in_order.head(); window; window = window->next()) {
        if (!window->is_visible())
            continue;
        if (window->type() != type)
            continue;
        if (callback(*window) == IterationDecision::Abort)
            return IterationDecision::Abort;
    }
    return IterationDecision::Continue;
}

template<typename Callback>
IterationDecision WSWindowManager::for_each_visible_window_from_back_to_front(Callback callback)
{
    if (for_each_visible_window_of_type_from_back_to_front(WSWindowType::Normal, callback) == IterationDecision::Abort)
        return IterationDecision::Abort;
    return for_each_visible_window_of_type_from_back_to_front(WSWindowType::Menu, callback);
}

template<typename Callback>
IterationDecision WSWindowManager::for_each_visible_window_of_type_from_front_to_back(WSWindowType type, Callback callback)
{
    for (auto* window = m_windows_in_order.tail(); window; window = window->prev()) {
        if (!window->is_visible())
            continue;
        if (window->type() != type)
            continue;
        if (callback(*window) == IterationDecision::Abort)
            return IterationDecision::Abort;
    }
    return IterationDecision::Continue;
}

template<typename Callback>
IterationDecision WSWindowManager::for_each_visible_window_from_front_to_back(Callback callback)
{
    if (for_each_visible_window_of_type_from_front_to_back(WSWindowType::Menu, callback) == IterationDecision::Abort)
        return IterationDecision::Abort;
    return for_each_visible_window_of_type_from_front_to_back(WSWindowType::Normal, callback);
}

void WSWindowManager::compose()
{
    auto dirty_rects = move(m_dirty_rects);
    auto cursor_location = m_screen.cursor_location();
    dirty_rects.add(m_last_cursor_rect);
    dirty_rects.add({ cursor_location.x(), cursor_location.y(), (int)m_cursor_bitmap_inner->width(), (int)m_cursor_bitmap_inner->height() });
#ifdef DEBUG_COUNTERS
    dbgprintf("[WM] compose #%u (%u rects)\n", ++m_compose_count, dirty_rects.rects().size());
#endif

    auto any_opaque_window_contains_rect = [this] (const Rect& r) {
        for (auto* window = m_windows_in_order.head(); window; window = window->next()) {
            if (!window->is_visible())
                continue;
            if (window->opacity() < 1.0f)
                continue;
            if (window->has_alpha_channel()) {
                // FIXME: Just because the window has an alpha channel doesn't mean it's not opaque.
                //        Maybe there's some way we could know this?
                continue;
            }
            if (outer_window_rect(window->rect()).contains(r))
                return true;
        }
        return false;
    };

    auto any_dirty_rect_intersects_window = [&dirty_rects] (const WSWindow& window) {
        auto window_rect = outer_window_rect(window.rect());
        for (auto& dirty_rect : dirty_rects.rects()) {
            if (dirty_rect.intersects(window_rect))
                return true;
        }
        return false;
    };

    for (auto& dirty_rect : dirty_rects.rects()) {
        if (any_opaque_window_contains_rect(dirty_rect))
            continue;
        if (!m_wallpaper)
            m_back_painter->fill_rect(dirty_rect, m_background_color);
        else
            m_back_painter->blit(dirty_rect.location(), *m_wallpaper, dirty_rect);
    }

    for_each_visible_window_from_back_to_front([&] (WSWindow& window) {
        RetainPtr<GraphicsBitmap> backing_store = window.backing_store();
        if (!backing_store)
            return IterationDecision::Continue;
        if (!any_dirty_rect_intersects_window(window))
            return IterationDecision::Continue;
        for (auto& dirty_rect : dirty_rects.rects()) {
            m_back_painter->set_clip_rect(dirty_rect);
            paint_window_frame(window);
            Rect dirty_rect_in_window_coordinates = Rect::intersection(dirty_rect, window.rect());
            if (dirty_rect_in_window_coordinates.is_empty())
                continue;
            dirty_rect_in_window_coordinates.set_x(dirty_rect_in_window_coordinates.x() - window.x());
            dirty_rect_in_window_coordinates.set_y(dirty_rect_in_window_coordinates.y() - window.y());
            auto dst = window.position();
            dst.move_by(dirty_rect_in_window_coordinates.location());
            if (window.opacity() == 1.0f)
                m_back_painter->blit(dst, *backing_store, dirty_rect_in_window_coordinates);
            else
                m_back_painter->blit_with_opacity(dst, *backing_store, dirty_rect_in_window_coordinates, window.opacity());
            m_back_painter->clear_clip_rect();
        }
        m_back_painter->clear_clip_rect();
        return IterationDecision::Continue;
    });

    draw_menubar();
    draw_cursor();

    if (m_flash_flush) {
        for (auto& rect : dirty_rects.rects())
            m_front_painter->fill_rect(rect, Color::Yellow);
    }

    flip_buffers();
    for (auto& r : dirty_rects.rects())
        flush(r);
}

void WSWindowManager::invalidate_cursor()
{
    auto cursor_location = m_screen.cursor_location();
    Rect cursor_rect { cursor_location.x(), cursor_location.y(), (int)m_cursor_bitmap_inner->width(), (int)m_cursor_bitmap_inner->height() };
    invalidate(cursor_rect);
}

Rect WSWindowManager::menubar_rect() const
{
    return { 0, 0, m_screen_rect.width(), 16 };
}

void WSWindowManager::draw_menubar()
{
    m_back_painter->fill_rect(menubar_rect(), Color::LightGray);
    m_back_painter->draw_line({ 0, menubar_rect().bottom() }, { menubar_rect().right(), menubar_rect().bottom() }, Color::White);
    for_each_active_menubar_menu([&] (WSMenu& menu) {
        Color text_color = Color::Black;
        if (&menu == current_menu()) {
            m_back_painter->fill_rect(menu.rect_in_menubar(), menu_selection_color());
            text_color = Color::White;
        }
        m_back_painter->draw_text(menu.text_rect_in_menubar(), menu.name(), TextAlignment::CenterLeft, text_color);
        return true;
    });

    time_t now = time(nullptr);
    auto* tm = localtime(&now);
    auto time_text = String::format("%4u-%02u-%02u %02u:%02u:%02u\n",
        tm->tm_year + 1900,
        tm->tm_mon + 1,
        tm->tm_mday,
        tm->tm_hour,
        tm->tm_min,
        tm->tm_sec);
    auto time_rect = menubar_rect().translated(-(menubar_menu_margin() / 2), 0);
    m_back_painter->draw_text(time_rect, time_text, TextAlignment::CenterRight, Color::Black);
}

void WSWindowManager::draw_cursor()
{
    auto cursor_location = m_screen.cursor_location();
    Rect cursor_rect { cursor_location.x(), cursor_location.y(), (int)m_cursor_bitmap_inner->width(), (int)m_cursor_bitmap_inner->height() };
    Color inner_color = Color::White;
    Color outer_color = Color::Black;
    if (m_screen.left_mouse_button_pressed())
        swap(inner_color, outer_color);
    m_back_painter->draw_bitmap(cursor_location, *m_cursor_bitmap_inner, inner_color);
    m_back_painter->draw_bitmap(cursor_location, *m_cursor_bitmap_outer, outer_color);
    m_last_cursor_rect = cursor_rect;
}

void WSWindowManager::on_message(WSMessage& message)
{
    if (message.is_mouse_event()) {
        WSWindow* event_window = nullptr;
        process_mouse_event(static_cast<WSMouseEvent&>(message), event_window);
        set_hovered_window(event_window);
        return;
    }

    if (message.is_key_event()) {
        // FIXME: This is a good place to hook key events globally. :)
        if (m_active_window)
            return m_active_window->on_message(message);
        return;
    }

    if (message.type() == WSMessage::WM_DeferredCompose) {
        m_pending_compose_event = false;
        compose();
        return;
    }
}

void WSWindowManager::set_active_window(WSWindow* window)
{
    if (window->type() == WSWindowType::Menu) {
        dbgprintf("WSWindowManager: Attempted to make a menu window active.\n");
        return;
    }

    if (window == m_active_window.ptr())
        return;

    if (auto* previously_active_window = m_active_window.ptr()) {
        WSMessageLoop::the().post_message(previously_active_window, make<WSMessage>(WSMessage::WindowDeactivated));
        invalidate(*previously_active_window);
    }
    m_active_window = window->make_weak_ptr();
    if (m_active_window) {
        WSMessageLoop::the().post_message(m_active_window.ptr(), make<WSMessage>(WSMessage::WindowActivated));
        invalidate(*m_active_window);

        auto* client = window->client();
        ASSERT(client);
        set_current_menubar(client->app_menubar());
    }
}

void WSWindowManager::set_hovered_window(WSWindow* window)
{
    if (m_hovered_window.ptr() == window)
        return;

    if (m_hovered_window)
        WSMessageLoop::the().post_message(m_hovered_window.ptr(), make<WSMessage>(WSMessage::WindowLeft));

    m_hovered_window = window ? window->make_weak_ptr() : nullptr;

    if (m_hovered_window)
        WSMessageLoop::the().post_message(m_hovered_window.ptr(), make<WSMessage>(WSMessage::WindowEntered));
}

void WSWindowManager::invalidate()
{
    m_dirty_rects.clear_with_capacity();
    invalidate(m_screen_rect);
}

void WSWindowManager::recompose_immediately()
{
    m_dirty_rects.clear_with_capacity();
    invalidate(m_screen_rect, false);
}

void WSWindowManager::invalidate(const Rect& a_rect, bool should_schedule_compose_event)
{
    auto rect = Rect::intersection(a_rect, m_screen_rect);
    if (rect.is_empty())
        return;

    m_dirty_rects.add(rect);

    if (should_schedule_compose_event && !m_pending_compose_event) {
        WSMessageLoop::the().post_message(this, make<WSMessage>(WSMessage::WM_DeferredCompose));
        m_pending_compose_event = true;
    }
}

void WSWindowManager::invalidate(const WSWindow& window)
{
    if (window.type() == WSWindowType::Menu) {
        invalidate(menu_window_rect(window.rect()));
        return;
    }
    if (window.type() == WSWindowType::Normal) {
        invalidate(outer_window_rect(window.rect()));
        return;
    }
    ASSERT_NOT_REACHED();
}

void WSWindowManager::invalidate(const WSWindow& window, const Rect& rect)
{
    if (rect.is_empty()) {
        invalidate(window);
        return;
    }
    auto outer_rect = outer_window_rect(window.rect());
    auto inner_rect = rect;
    inner_rect.move_by(window.position());
    // FIXME: This seems slightly wrong; the inner rect shouldn't intersect the border part of the outer rect.
    inner_rect.intersect(outer_rect);
    invalidate(inner_rect);
}

void WSWindowManager::flush(const Rect& a_rect)
{
    auto rect = Rect::intersection(a_rect, m_screen_rect);

#ifdef DEBUG_COUNTERS
    dbgprintf("[WM] flush #%u (%d,%d %dx%d)\n", ++m_flush_count, rect.x(), rect.y(), rect.width(), rect.height());
#endif

    RGBA32* front_ptr = m_front_bitmap->scanline(rect.y()) + rect.x();
    RGBA32* back_ptr = m_back_bitmap->scanline(rect.y()) + rect.x();
    size_t pitch = m_back_bitmap->pitch();

    for (int y = 0; y < rect.height(); ++y) {
        fast_dword_copy(back_ptr, front_ptr, rect.width());
        front_ptr = (RGBA32*)((byte*)front_ptr + pitch);
        back_ptr = (RGBA32*)((byte*)back_ptr + pitch);
    }
}

void WSWindowManager::close_menu(WSMenu& menu)
{
    if (current_menu() == &menu)
        close_current_menu();
}

void WSWindowManager::close_menubar(WSMenuBar& menubar)
{
    if (current_menubar() == &menubar)
        set_current_menubar(nullptr);
}

const WSClientConnection* WSWindowManager::active_client() const
{
    if (m_active_window)
        return m_active_window->client();
    return 0;
}

void WSWindowManager::notify_client_changed_app_menubar(WSClientConnection& client)
{
    if (active_client() == &client)
        set_current_menubar(client.app_menubar());
    invalidate();
}
