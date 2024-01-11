#ifndef ZWM_ZWM_H
#define ZWM_ZWM_H

#include "type.h"

int                       handle_first_window(client_t *new_client, wm_t *wm);
int                       handle_subsequent_window(client_t *new_client, wm_t *wm);
xcb_get_geometry_reply_t *get_geometry(xcb_window_t win, xcb_connection_t *c);
client_t                 *create_client(xcb_window_t win, xcb_atom_t wtype, xcb_connection_t *cn);
void                      init_clients();
int                       kill_window(xcb_connection_t *conn, xcb_window_t win, node_t *root, wm_t *wm);
void                      add_client(client_t *new_client);
void                      free_clients();
wm_t                     *init_wm();
int                       resize_window(wm_t *wm, xcb_window_t win, uint16_t width, uint16_t height);
int                       move_window(wm_t *wm, xcb_window_t win, uint16_t x, uint16_t y);
int16_t                   get_cursor_axis(xcb_connection_t *conn, xcb_window_t window);
int                       handle_map_request(xcb_window_t win, wm_t *wm);
int                       handle_enter_notify(xcb_connection_t *conn, xcb_window_t win, node_t *root);
client_t                 *find_client_by_window(xcb_window_t win);
int                       tile(wm_t *wm, node_t *node);
int                       handle_leave_notify(xcb_connection_t *conn, xcb_window_t win, node_t *root);
int                       change_border_attributes(xcb_connection_t *cn, client_t *cl, uint32_t b_color, uint32_t b_width, bool stack);
int 					  change_window_attributes(xcb_connection_t *conn, xcb_window_t window, uint32_t attribute, const void *);
int 					  configure_window(xcb_connection_t *conn, xcb_window_t window, uint16_t attribute, const void *);
int 					  set_input_focus(xcb_connection_t *conn, uint8_t revert_to, xcb_window_t window, xcb_timestamp_t time);
int 					  handle_xcb_error(xcb_connection_t *conn, xcb_void_cookie_t cookie, const char *err_message);
#endif // ZWM_ZWM_H