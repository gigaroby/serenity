#pragma once

#include "GObject.h"
#include <SharedGraphics/Rect.h>
#include <SharedGraphics/GraphicsBitmap.h>
#include <AK/AKString.h>
#include <AK/WeakPtr.h>

class GWidget;

class GWindow final : public GObject {
public:
    GWindow(GObject* parent = nullptr);
    virtual ~GWindow() override;

    static GWindow* from_window_id(int);

    void set_has_alpha_channel(bool);
    void set_opacity(float);

    int window_id() const { return m_window_id; }

    String title() const;
    void set_title(String&&);

    int x() const { return rect().x(); }
    int y() const { return rect().y(); }
    int width() const { return rect().width(); }
    int height() const { return rect().height(); }

    Rect rect() const;
    Size size() const { return rect().size(); }
    void set_rect(const Rect&);
    void set_rect(int x, int y, int width, int height) { set_rect({ x, y, width, height }); }

    Point position() const { return rect().location(); }

    void move_to(int x, int y) { move_to({ x, y }); }
    void move_to(const Point& point) { set_rect({ point, size() }); }

    virtual void event(GEvent&) override;

    bool is_visible() const;
    bool is_active() const { return m_is_active; }

    void show();
    void hide();
    void close();

    GWidget* main_widget() { return m_main_widget; }
    const GWidget* main_widget() const { return m_main_widget; }
    void set_main_widget(GWidget*);

    GWidget* focused_widget() { return m_focused_widget; }
    const GWidget* focused_widget() const { return m_focused_widget; }
    void set_focused_widget(GWidget*);

    void update(const Rect& = Rect());

    void set_global_cursor_tracking_widget(GWidget*);
    GWidget* global_cursor_tracking_widget() { return m_global_cursor_tracking_widget.ptr(); }
    const GWidget* global_cursor_tracking_widget() const { return m_global_cursor_tracking_widget.ptr(); }

    bool should_exit_app_on_close() const { return m_should_exit_app_on_close; }
    void set_should_exit_app_on_close(bool b) { m_should_exit_app_on_close = b; }

    GWidget* hovered_widget() { return m_hovered_widget.ptr(); }
    const GWidget* hovered_widget() const { return m_hovered_widget.ptr(); }
    void set_hovered_widget(GWidget*);

    GraphicsBitmap* backing() { return m_backing.ptr(); }

    Size size_increment() const { return m_size_increment; }
    void set_size_increment(const Size& increment) { m_size_increment = increment; }
    Size base_size() const { return m_base_size; }
    void set_base_size(const Size& size) { m_base_size = size; }

private:
    virtual const char* class_name() const override { return "GWindow"; }

    RetainPtr<GraphicsBitmap> m_backing;
    int m_window_id { 0 };
    float m_opacity_when_windowless { 1.0f };
    GWidget* m_main_widget { nullptr };
    GWidget* m_focused_widget { nullptr };
    WeakPtr<GWidget> m_global_cursor_tracking_widget;
    WeakPtr<GWidget> m_hovered_widget;
    Rect m_rect_when_windowless;
    String m_title_when_windowless;
    Vector<Rect> m_pending_paint_event_rects;
    Size m_size_increment;
    Size m_base_size;
    bool m_is_active { false };
    bool m_should_exit_app_on_close { false };
    bool m_has_alpha_channel { false };
};

