/*
    This file is part of the Ansel project.
    Copyright (C) 2025 Aurélien PIERRE.
    Copyright (C) 2026 Guillaume Stutin.
    Copyright (C) 2026 Roland Sahli.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/logging.h"
#include "colorprofiles/colorspaces.h"   // dt_colorprofiles_xyz_to_display()
#include "common/colorspaces_inline_conversions.h"   // dt_Lab_to_XYZ
#include "common/conf.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "system/simd.h"
#include "common/times.h"
#include "control/signal.h"
#include "gui/application.h"
#include "control/control.h"
#include "control/user_message.h"
#include "caches/image_cache.h"
#include "views/view.h"
#include "gui/dtgtk/preview_window.h"

#include <gtk/gtk.h>
#include "widgets/widget_settings.h"

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

typedef struct dt_preview_window_t
{
  int32_t imgid;
  cairo_surface_t *surface;
  dt_view_image_surface_fetcher_t fetcher;
  GtkWidget *area;
  GtkWidget *window;
  int width;
  int height;
  gboolean placed; // initial placement done: geometry changes now come from the user
  gboolean has_position; // restore_x/y were taken from the saved geometry at creation
  int restore_x, restore_y; // kept here: other previews' configure events rewrite the saved geometry meanwhile
  GdkMonitor *monitor; // the monitor this window currently sits on (identity only, never dereferenced once gone)
  gboolean evacuate; // its monitor was unplugged: to be reopened on the main monitor
  GdkMonitor *previous_monitor; // the one before, as the OS may have moved us away from an unplugged one
} dt_preview_window_t;

void _preview_redraw(gpointer instance, dt_preview_window_t *preview);

typedef struct dt_preview_geometry_t
{
  gboolean valid;
  int x, y, width, height;
  GdkRectangle monitor; // geometry of the monitor the window was saved on
} dt_preview_geometry_t;

/** Last restored (non-maximized) geometry of any preview this session, reused by the next ones. */
static dt_preview_geometry_t _preview_last = { 0 };

/**
 * @brief Loop on the connected monitors, looking for the one a geometry was saved on.
 * Monitors are compared by geometry, so a monitor that is plugged back in matches again.
 */
static gboolean _preview_monitor_connected(GdkDisplay *display, const dt_preview_geometry_t *saved)
{
  const int n = gdk_display_get_n_monitors(display);
  for(int i = 0; i < n; i++)
  {
    GdkRectangle geometry = { 0 };
    gdk_monitor_get_geometry(gdk_display_get_monitor(display, i), &geometry);
    if(!memcmp(&geometry, &saved->monitor, sizeof(GdkRectangle))) return TRUE;
  }
  return FALSE;
}

/**
 * @brief Make the saved geometry a default-size preview centered over the main window,
 * as on a first preview. Every preview sent back from an unplugged monitor gets this same geometry.
 */
static void _preview_center_last(void)
{
  GtkWidget *main_window = dt_gui_main_window();
  GdkWindow *main_gdk_window = gtk_widget_get_window(main_window);
  int main_x = 0, main_y = 0;
  gdk_window_get_origin(main_gdk_window, &main_x, &main_y);

  _preview_last.width = 420;
  _preview_last.height = 420;
  _preview_last.x = main_x + (gtk_widget_get_allocated_width(main_window) - _preview_last.width) / 2;
  _preview_last.y = main_y + (gtk_widget_get_allocated_height(main_window) - _preview_last.height) / 2;
  gdk_monitor_get_geometry(gdk_display_get_monitor_at_window(gdk_window_get_display(main_gdk_window), main_gdk_window),
                           &_preview_last.monitor);
  _preview_last.valid = TRUE;
}

/** All the open previews, to find those sitting on an unplugged monitor. */
static GList *_preview_windows = NULL;
static guint _preview_evacuate_source = 0;

/**
 * @brief Reopen every marked preview as a new window, centered over the main window at the default size.
 * Moving the existing windows instead let the OS restore their placement on the external monitor when it
 * is plugged back in, slowly. Done from an idle callback, once, and not from the monitor signal, because
 * destroying windows while the OS is rearranging them minimized Ansel. Each new window is opened before
 * its predecessor is destroyed, so that Ansel never loses its last owned window.
 */
static gboolean _preview_evacuate(gpointer user_data)
{
  _preview_evacuate_source = 0;
#ifdef GDK_WINDOWING_WIN32
  // Windows minimizes Ansel on a monitor change, and hides all the previews it owns with it.
  gtk_window_present(GTK_WINDOW(dt_gui_main_window()));
#endif

  // Loop on a snapshot: opening a preview appends to _preview_windows.
  GList *snapshot = g_list_copy(_preview_windows);
  for(GList *l = snapshot; l; l = g_list_next(l))
  {
    dt_preview_window_t *preview = (dt_preview_window_t *)l->data;
    if(!preview->evacuate) continue;
    _preview_center_last();
    dt_preview_window_spawn(preview->imgid);
    gtk_widget_destroy(preview->window);
  }
  g_list_free(snapshot);
  return G_SOURCE_REMOVE;
}

#ifdef GDK_WINDOWING_WIN32
/** @brief A monitor was plugged back in: Windows minimizes Ansel again, restore it. */
static void _preview_monitor_added(GdkDisplay *display, GdkMonitor *monitor, gpointer user_data)
{
  if(_preview_evacuate_source == 0) _preview_evacuate_source = g_idle_add(_preview_evacuate, NULL);
}
#endif

/**
 * @brief Mark the preview if it sat on the unplugged monitor. The OS may already have moved it away
 * before this signal, hence the previous monitor too. Monitors are compared by pointer: the OS also
 * rearranges the geometry of the remaining ones.
 */
static void _preview_monitor_removed(GdkDisplay *display, GdkMonitor *monitor, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  if(monitor != preview->monitor && monitor != preview->previous_monitor) return;

  preview->evacuate = TRUE;
  if(_preview_evacuate_source == 0) _preview_evacuate_source = g_idle_add(_preview_evacuate, NULL);
}

static void _preview_window_destroy(GtkWidget *dialog, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  // Disconnect from DARKROOM_UI_CHANGED before freeing: otherwise the handler keeps
  // firing with a dangling `preview` pointer and crashes on the next UI change
  // (e.g. moving the border-size slider).
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_preview_redraw), preview);
  g_signal_handlers_disconnect_by_data(gtk_widget_get_display(dialog), preview);
  _preview_windows = g_list_remove(_preview_windows, preview);
  dt_view_image_surface_fetcher_cleanup(&preview->fetcher);
  dt_free(preview);
}

/** @brief Close on Escape or on the close button, and give the focus back to the main window. */
static gboolean _preview_close(GtkWidget *window)
{
  gtk_widget_destroy(window);
  gtk_window_present(GTK_WINDOW(dt_gui_main_window()));
  return TRUE;
}

static gboolean _preview_delete_event(GtkWidget *window, GdkEvent *event, gpointer user_data)
{
  return _preview_close(window);
}

static gboolean _preview_key_press_event(GtkWidget *window, GdkEventKey *event, gpointer user_data)
{
  return event->keyval == GDK_KEY_Escape ? _preview_close(window) : FALSE;
}

/**
 * @brief Remember the restored geometry, so that the next previews open the same way.
 * Maximized and fullscreen states are skipped: their size is not the restored one.
 */
static gboolean _preview_configure_event(GtkWidget *window, GdkEventConfigure *event, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  GdkWindow *gdk_window = gtk_widget_get_window(window);
  if(!preview->placed || IS_NULL_PTR(gdk_window)) return FALSE;

  GdkDisplay *display = gtk_widget_get_display(window);
  GdkMonitor *monitor = gdk_display_get_monitor_at_window(display, gdk_window);
  if(monitor != preview->monitor)
  {
    preview->previous_monitor = preview->monitor;
    preview->monitor = monitor;
  }
  if(gdk_window_get_state(gdk_window) & (GDK_WINDOW_STATE_MAXIMIZED | GDK_WINDOW_STATE_FULLSCREEN)) return FALSE;

  gdk_monitor_get_geometry(monitor, &_preview_last.monitor);
  gtk_window_get_position(GTK_WINDOW(window), &_preview_last.x, &_preview_last.y);
  gtk_window_get_size(GTK_WINDOW(window), &_preview_last.width, &_preview_last.height);
  _preview_last.valid = TRUE;

  return FALSE;
}

/** @brief On Windows, keep the preview from being minimized by the taskbar or Win+D. */
static gboolean _preview_window_state_event(GtkWidget *window, GdkEventWindowState *event, gpointer user_data)
{
#ifdef GDK_WINDOWING_WIN32
  // The minimize box is removed on map, but the taskbar and Win+D can still iconify the window.
  if(event->new_window_state & GDK_WINDOW_STATE_ICONIFIED)
  {
    gtk_window_deiconify(GTK_WINDOW(window));
    return TRUE;
  }
#endif
  return FALSE;
}

/**
 * @brief Once mapped, re-apply the restored position (some window managers ignore the pre-map request)
 * and, on Windows, drop the minimize button: GTK recomputes the native style when mapping the window,
 * so it can only be removed afterwards.
 */
static void _preview_map_event(GtkWidget *window, GdkEvent *event, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
#ifdef GDK_WINDOWING_WIN32
  // Let GDK own the native style: it re-applies it on every maximize/restore, which
  // silently brought the minimize box back and broke the restore button when set by hand.
  gdk_window_set_functions(gtk_widget_get_window(window),
                           GDK_FUNC_MOVE | GDK_FUNC_RESIZE | GDK_FUNC_MAXIMIZE | GDK_FUNC_CLOSE);
#endif
  if(!preview->placed && preview->has_position)
    gtk_window_move(GTK_WINDOW(window), preview->restore_x, preview->restore_y);
  preview->placed = TRUE;
}

static void _preview_window_size_allocate(GtkWidget *widget, GtkAllocation *allocation, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  if(IS_NULL_PTR(preview) || IS_NULL_PTR(allocation)) return;
  if(allocation->width < 2 || allocation->height < 2) return;
  if(preview->width == allocation->width && preview->height == allocation->height) return;

  preview->width = allocation->width;
  preview->height = allocation->height;

  /* The async fetcher can stop its current pixelpipe through the request-owned
   * shutdown flag. Trigger that as soon as the popup size changes so we don't
   * keep rendering a surface for an obsolete allocation. */
  dt_view_image_surface_fetcher_invalidate(&preview->fetcher, &preview->surface);
  gtk_widget_queue_draw(widget);
}

// TODO: we color-manage the preview window assuming it sits on the same screen as the main window
// Will need to grab the color profile of the actual monitor where it sits.
void _colormanage_ui_color(const float L, const float a, const float b, dt_aligned_pixel_t RGB)
{
  dt_aligned_pixel_t Lab = { L, a, b, 1.f };
  dt_aligned_pixel_t XYZ = { 0.f, 0.f, 0.f, 1.f };
  dt_Lab_to_XYZ(Lab, XYZ);
  dt_colorprofiles_xyz_to_display(XYZ, RGB);
}

static gboolean
_thumb_draw_image(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  if(IS_NULL_PTR(preview)) return TRUE;

  const double start = dt_get_wtime();

  int w = gtk_widget_get_allocated_width(widget);
  int h = gtk_widget_get_allocated_height(widget);

  // Paint background
  dt_aligned_pixel_t bg_color;
  _colormanage_ui_color((float)dt_conf_get_int("display/brightness"), 0., 0., bg_color);
  cairo_set_source_rgb(cr, bg_color[0], bg_color[1], bg_color[2]);
  cairo_paint(cr);

  int borders = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
  w -= 2 * borders;
  h -= 2 * borders;

  // FIXME: this gets the color-managed image surface using the color profile of the monitor
  // where the main window sits. Need to detect the monitor of the preview window instead.
  const dt_view_surface_value_t res =
      dt_view_image_get_surface_async(&preview->fetcher, preview->imgid, w, h, &preview->surface, widget, 0);

  if(preview->surface && res == DT_VIEW_SURFACE_OK)
  {
    // The image is immediately available
    int width = cairo_image_surface_get_width(preview->surface);
    int height = cairo_image_surface_get_height(preview->surface);
    double sx = 1.0, sy = 1.0;
    cairo_surface_get_device_scale(preview->surface, &sx, &sy);
    const double logical_width = width / sx;
    const double logical_height = height / sy;

    // we draw the image
    cairo_save(cr);
    double x_offset = (w - logical_width) / 2. + borders;
    double y_offset = (h - logical_height) / 2. + borders;
    cairo_set_source_surface(cr, preview->surface, x_offset, y_offset);

    // get the transparency value
    GdkRGBA im_color;
    GtkStyleContext *context = gtk_widget_get_style_context(widget);
    gtk_style_context_get_color(context, gtk_widget_get_state_flags(widget), &im_color);
    cairo_paint_with_alpha(cr, im_color.alpha);

    // and eventually the image border
    gtk_render_frame(context, cr, borders, borders, w, h);
    cairo_restore(cr);
  }
  else
  {
    dt_control_draw_busy_msg(cr, w, h);
  }

  dt_print(DT_DEBUG_LIGHTTABLE, "Redrawing the preview window for %i in %0.04f sec\n", preview->imgid,
    dt_get_wtime() - start);

  return TRUE;
}


void _preview_redraw(gpointer instance, dt_preview_window_t *preview)
{
  gtk_widget_queue_draw(preview->area);
}


/**
 * @brief Open an image preview, restoring the geometry of the previous one if any.
 * @param imgid Image to display.
 */
void dt_preview_window_spawn(const int32_t imgid)
{
  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  if(IS_NULL_PTR(img))
  {
    dt_control_log(_("No image to open !"));
    return;
  }
  gchar *name = g_strdup_printf(_("Ansel - Preview: %s [%d]"), img->filename, img->version);
  dt_image_cache_read_release(img);

  dt_preview_window_t *preview = calloc(1, sizeof(dt_preview_window_t));
  preview->imgid = imgid;
  dt_view_image_surface_fetcher_init(&preview->fetcher);

  // A plain top-level window rather than a GtkDialog, to get the maximize button.
  GtkWidget *dialog = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  preview->window = dialog;
  gtk_window_set_title(GTK_WINDOW(dialog), name);
  dt_free(name);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif

  gtk_window_set_modal(GTK_WINDOW(dialog), FALSE);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(dt_gui_main_window()));
  gtk_window_set_skip_taskbar_hint(GTK_WINDOW(dialog), TRUE);

  // A saved position on a monitor that is gone would be unreachable: start afresh.
  if(_preview_last.valid && !_preview_monitor_connected(gtk_widget_get_display(dialog), &_preview_last))
    _preview_center_last();

  // Without a restored position, the window manager centers it over the main window.
  if(!_preview_last.valid)
    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
  preview->has_position = _preview_last.valid;
  preview->restore_x = _preview_last.x;
  preview->restore_y = _preview_last.y;
  gtk_window_set_default_size(GTK_WINDOW(dialog), _preview_last.valid ? _preview_last.width : 420,
                              _preview_last.valid ? _preview_last.height : 420);

  g_signal_connect(G_OBJECT(dialog), "destroy", G_CALLBACK(_preview_window_destroy), preview);
  g_signal_connect(G_OBJECT(dialog), "delete-event", G_CALLBACK(_preview_delete_event), preview);
  g_signal_connect(G_OBJECT(dialog), "key-press-event", G_CALLBACK(_preview_key_press_event), preview);
  g_signal_connect(G_OBJECT(dialog), "configure-event", G_CALLBACK(_preview_configure_event), preview);
  g_signal_connect(G_OBJECT(dialog), "window-state-event", G_CALLBACK(_preview_window_state_event), preview);
  g_signal_connect(G_OBJECT(dialog), "map-event", G_CALLBACK(_preview_map_event), preview);
  _preview_windows = g_list_append(_preview_windows, preview);
#ifdef GDK_WINDOWING_WIN32
  g_signal_connect(gtk_widget_get_display(dialog), "monitor-added", G_CALLBACK(_preview_monitor_added), preview);
#endif
  g_signal_connect(gtk_widget_get_display(dialog), "monitor-removed", G_CALLBACK(_preview_monitor_removed), preview);


  preview->area = gtk_drawing_area_new();
  gtk_widget_set_hexpand(preview->area, TRUE);
  gtk_widget_set_vexpand(preview->area, TRUE);
  gtk_widget_set_size_request(preview->area, 120, 120);
  gtk_container_add(GTK_CONTAINER(dialog), preview->area);
  g_signal_connect(G_OBJECT(preview->area), "draw", G_CALLBACK(_thumb_draw_image), preview);
  g_signal_connect(G_OBJECT(preview->area), "size-allocate", G_CALLBACK(_preview_window_size_allocate), preview);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_DARKROOM_UI_CHANGED,
                                  G_CALLBACK(_preview_redraw), preview);

  gtk_widget_show_all(dialog);
  gtk_window_present(GTK_WINDOW(dialog)); // above the previews already open
}