#include "zwm.h"
#include "logger.h"
#include "tree.h"
#include "type.h"
#include <X11/keysym.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wait.h>
#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xproto.h>

#ifdef DEBUGGING
#define DEBUG(x)	   fprintf(stderr, "%s\n", x);
#define DEBUGP(x, ...) fprintf(stderr, x, ##__VA_ARGS__);
#else
#define DEBUG(x)
#define DEBUGP(x, ...)
#endif

// handy xcb masks for common operations
#define XCB_MOVE_RESIZE                                                        \
	(XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |     \
	 XCB_CONFIG_WINDOW_HEIGHT)
#define XCB_MOVE   (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y)
#define XCB_RESIZE (XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT)
#define SUBSTRUCTURE_REDIRECTION                                               \
	(XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT)
#define CLIENT_EVENT_MASK                                                      \
	(XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_FOCUS_CHANGE |            \
	 XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW |               \
	 XCB_KEY_PRESS | XCB_KEY_RELEASE)
#define ROOT_EVENT_MASK	   (SUBSTRUCTURE_REDIRECTION)

#define LENGTH(x)		   (sizeof(x) / sizeof(*x))
#define CLEANMASK(mask)	   (mask & ~(0 | XCB_MOD_MASK_LOCK))
#define NUMBER_OF_DESKTOPS 5
#define WM_NAME			   "zwm"
#define WM_CLASS_NAME	   "null"
#define WM_INSTANCE_NAME   "null"
#define ALT_MASK		   XCB_MOD_MASK_1
#define SUPER_MASK		   XCB_MOD_MASK_4
#define SHIFT_MASK		   XCB_MOD_MASK_SHIFT
#define CTRL_MASK		   XCB_MOD_MASK_CONTROL

wm_t		*wm	  = NULL;
xcb_window_t wbar = XCB_NONE;
config_t	 conf = {0};

// clang-format off
static _key__t keys_[] = {
	{SUPER_MASK,              XK_w,      close_or_kill,             &((arg_t){0})                 },
	{SUPER_MASK,              XK_Return, exec_process,              &((arg_t){.cmd = "alacritty"})},
	{SUPER_MASK,              XK_space,  exec_process,              &((arg_t){.cmd = "dmenu_run"})},
	{SUPER_MASK,              XK_1,      switch_desktop_wrapper,    &((arg_t){.idx = 0})          },
	{SUPER_MASK,              XK_2,      switch_desktop_wrapper,    &((arg_t){.idx = 1})          },
	{SUPER_MASK,              XK_3,      switch_desktop_wrapper,    &((arg_t){.idx = 2})          },
	{SUPER_MASK,              XK_4,      switch_desktop_wrapper,    &((arg_t){.idx = 3})          },
	{SUPER_MASK,              XK_5,      switch_desktop_wrapper,    &((arg_t){.idx = 4})          },
	{SUPER_MASK,              XK_l,      horizontal_resize_wrapper, &((arg_t){.r = GROW})         },
	{SUPER_MASK,              XK_h,      horizontal_resize_wrapper, &((arg_t){.r = SHRINK})       },
	{SUPER_MASK,              XK_f,      set_fullscreen_wrapper,    &((arg_t){0})                 },
	{SUPER_MASK | SHIFT_MASK, XK_1,      transfer_node_wrapper,     &((arg_t){.idx = 0})          },
	{SUPER_MASK | SHIFT_MASK, XK_2,      transfer_node_wrapper,     &((arg_t){.idx = 1})          },
	{SUPER_MASK | SHIFT_MASK, XK_3,      transfer_node_wrapper,     &((arg_t){.idx = 2})          },
	{SUPER_MASK | SHIFT_MASK, XK_4,      transfer_node_wrapper,     &((arg_t){.idx = 3})          },
	{SUPER_MASK | SHIFT_MASK, XK_5,      transfer_node_wrapper,     &((arg_t){.idx = 4})          },
	{SUPER_MASK, XK_p},
};

int	len_of_digits(long n) {
	 int count = 0;
	 do {
		 n /= 10;
		 ++count;
	 } while (n != 0);
	 return count;
}
// clang-format on

int			 check_xcb_error(xcb_conn_t *conn, xcb_void_cookie_t cookie,
							 const char *err) {
	 xcb_generic_error_t *error = xcb_request_check(conn, cookie);
	 if (error != NULL) {
		 log_message(ERROR, "%s: error code %d\n", err, error->error_code);
		 free(error);
		 return -1;
	 }
	 return 0;
}

xcb_ewmh_connection_t *ewmh_init(xcb_conn_t *conn) {
	if (conn == 0x00) {
		log_message(ERROR, "Connection is NULL");
		return NULL;
	}

	xcb_ewmh_connection_t *ewmh = calloc(1, sizeof(xcb_ewmh_connection_t));
	if (ewmh == NULL) {
		log_message(ERROR, "Cannot calloc ewmh");
		return NULL;
	}

	xcb_intern_atom_cookie_t *c = xcb_ewmh_init_atoms(conn, ewmh);
	if (c == 0x00) {
		log_message(ERROR, "Cannot init intern atom");
		return NULL;
	}

	const uint8_t res = xcb_ewmh_init_atoms_replies(ewmh, c, NULL);
	if (res != 1) {
		log_message(ERROR, "Cannot init intern atom");
		return NULL;
	}
	return ewmh;
}

int ewmh_set_supporting(xcb_window_t win, xcb_ewmh_connection_t *ewmh) {
	pid_t			  wm_pid = getpid();
	xcb_void_cookie_t supporting_cookie =
		xcb_ewmh_set_supporting_wm_check_checked(ewmh, win, win);
	xcb_void_cookie_t name_cookie =
		xcb_ewmh_set_wm_name_checked(ewmh, win, strlen(WM_NAME), WM_NAME);
	xcb_void_cookie_t pid_cookie =
		xcb_ewmh_set_wm_pid_checked(ewmh, win, (uint32_t)wm_pid);

	xcb_generic_error_t *err;
	if ((err = xcb_request_check(ewmh->connection, supporting_cookie))) {
		log_message(
			ERROR, "Error setting supporting window: %d\n", err->error_code);
		free(err);
		return -1;
	}
	if ((err = xcb_request_check(ewmh->connection, name_cookie))) {
		fprintf(stderr, "Error setting WM name: %d\n", err->error_code);
		free(err);
		return -1;
	}
	if ((err = xcb_request_check(ewmh->connection, pid_cookie))) {
		fprintf(stderr, "Error setting WM PID: %d\n", err->error_code);
		free(err);
		return -1;
	}

	return 0;
}

int get_focused_desktop_idx() {
	for (int i = 0; i < wm->n_of_desktops; ++i) {
		if (wm->desktops[i]->is_focused) {
			return wm->desktops[i]->id;
		}
	}
	return -1;
}

desktop_t *get_focused_desktop() {
	for (int i = 0; i < wm->n_of_desktops; ++i) {
		if (wm->desktops[i]->is_focused) {
			return wm->desktops[i];
		}
	}
	return NULL;
}

int ewmh_set_number_of_desktops(xcb_ewmh_connection_t *ewmh, int screen_nbr,
								uint32_t nd) {
	xcb_void_cookie_t cookie =
		xcb_ewmh_set_number_of_desktops_checked(ewmh, screen_nbr, nd);
	xcb_generic_error_t *err = xcb_request_check(ewmh->connection, cookie);
	if (err) {
		log_message(
			ERROR, "Error setting number of desktops: %d\n", err->error_code);
		free(err);
		return -1;
	}

	return 0;
}

int ewmh_update_desktop_names() {
	char		 names[MAXLEN];
	uint32_t	 names_len = 0;
	unsigned int offset	   = 0;

	for (int i = 0; i < wm->n_of_desktops; i++) {
		desktop_t *d = wm->desktops[i];
		for (int j = 0; d->name[j] != '\0' && (offset + j) < sizeof(names);
			 j++) {
			names[offset + j] = d->name[j];
		}
		offset += strlen(d->name);
		if (offset < sizeof(names)) {
			names[offset++] = '\0';
		}
	}

	names_len			= offset - 1;
	xcb_void_cookie_t c = xcb_ewmh_set_desktop_names_checked(
		wm->ewmh, wm->screen_nbr, names_len, names);
	xcb_generic_error_t *err = xcb_request_check(wm->ewmh->connection, c);
	if (err) {
		log_message(
			ERROR, "Error setting names of desktops: %d\n", err->error_code);
		free(err);
		return -1;
	}
	return 0;
}

int ewmh_set_active_win(xcb_ewmh_connection_t *ewmh, int screen_nbr,
						xcb_window_t win) {
	xcb_void_cookie_t c =
		xcb_ewmh_set_active_window_checked(ewmh, screen_nbr, win);
	xcb_generic_error_t *err = xcb_request_check(ewmh->connection, c);
	if (err) {
		fprintf(
			stderr, "Error setting number of desktops: %d\n", err->error_code);
		free(err);
		return -1;
	}
	return 0;
}

void remove_property(xcb_connection_t *con, xcb_window_t win, xcb_atom_t prop,
					 xcb_atom_t atom) {
	xcb_grab_server(con);

	xcb_get_property_reply_t *reply = xcb_get_property_reply(
		con,
		xcb_get_property(
			con, false, win, prop, XCB_GET_PROPERTY_TYPE_ANY, 0, 4096),
		NULL);
	if (reply == NULL || xcb_get_property_value_length(reply) == 0)
		goto release_grab;
	xcb_atom_t *atoms = xcb_get_property_value(reply);
	if (atoms == NULL) {
		goto release_grab;
	}

	{
		int		  num = 0;
		const int current_size =
			xcb_get_property_value_length(reply) / (reply->format / 8);
		xcb_atom_t values[current_size];
		for (int i = 0; i < current_size; i++) {
			if (atoms[i] != atom) values[num++] = atoms[i];
		}

		xcb_change_property(con,
							XCB_PROP_MODE_REPLACE,
							win,
							prop,
							XCB_ATOM_ATOM,
							32,
							num,
							values);
	}

release_grab:
	if (reply) free(reply);
	xcb_ungrab_server(con);
}

void lower_window(xcb_connection_t *conn, xcb_window_t win) {
	uint32_t		  values[] = {XCB_STACK_MODE_BELOW};
	uint16_t		  mask	   = XCB_CONFIG_WINDOW_STACK_MODE;

	xcb_void_cookie_t cookie =
		xcb_configure_window_checked(conn, win, mask, values);
	xcb_generic_error_t *err = xcb_request_check(conn, cookie);
	if (err != NULL) {
		log_message(ERROR,
					"Errror in stacking window %d: error code %d",
					win,
					err->error_code);
		free(err);
		// return -1;
	}
}

void raise_window(xcb_connection_t *conn, xcb_window_t win) {
	uint32_t		  values[] = {XCB_STACK_MODE_ABOVE};
	uint16_t		  mask	   = XCB_CONFIG_WINDOW_STACK_MODE;

	xcb_void_cookie_t cookie =
		xcb_configure_window_checked(conn, win, mask, values);
	xcb_generic_error_t *err = xcb_request_check(conn, cookie);
	if (err != NULL) {
		log_message(ERROR,
					"Errror in stacking window %d: error code %d",
					win,
					err->error_code);
		free(err);
		// return;
	}
}
int set_fullscreen_wrapper(arg_t *arg) {
	const int i = get_focused_desktop_idx();
	if (i == -1) {
		return -1;
	}
	node_t *root = wm->desktops[i]->tree;
	if (root == NULL) {
		return -1;
	}
	xcb_window_t w = get_window_under_cursor(wm->connection, wm->root_window);

	node_t		*n = find_node_by_window_id(root, w);
	 n->client->state == FULLSCREEN ? set_fullscreen(n, false)
										: set_fullscreen(n, true);
	return 0;
}

int set_fullscreen(node_t *n, bool flag) {
	if (n == NULL) {
		return -1;
	}

	rectangle_t r = {0};

	if (flag) {
		// long data[]		 = {wm->ewmh->_NET_WM_STATE_FULLSCREEN};
		r.x				 = 0;
		r.y				 = 0;
		r.width			 = wm->screen->width_in_pixels;
		r.height		 = wm->screen->height_in_pixels;
		n->client->state = FULLSCREEN;
		if (change_border_attr(
				wm->connection, n->client->window, 0x000000, 0, false) != 0) {
			return -1;
		}
		if (resize_window(n->client->window, r.width, r.height) != 0 ||
			move_window(n->client->window, r.x, r.y) != 0) {
			return -1;
		}
		// xcb_void_cookie_t c =
		// 	xcb_change_property_checked(wm->connection,
		// 								XCB_PROP_MODE_REPLACE,
		// 								n->client->window,
		// 								wm->ewmh->_NET_WM_STATE,
		// 								XCB_ATOM_ATOM,
		// 								32,
		// 								true,
		// 								data);
		// xcb_generic_error_t *err = xcb_request_check(wm->connection, c);
		// if (err) {
		// 	log_message(
		// 		ERROR, "Error changing window property: %d\n", err->error_code);
		// 	free(err);
		// 	return -1;
		// }
		goto out;
	}

	r				 = n->rectangle;
	n->client->state = TILED;
	if (resize_window(n->client->window, r.width, r.height) != 0 ||
		move_window(n->client->window, r.x, r.y) != 0) {
		return -1;
	}
	// remove_property(wm->connection,
	// 				n->client->window,
	// 				wm->ewmh->_NET_WM_STATE,
	// 				wm->ewmh->_NET_WM_STATE_FULLSCREEN);
	// xcb_void_cookie_t c = xcb_delete_property_checked(
	// 	wm->connection, n->client->window, wm->ewmh->_NET_WM_STATE_FULLSCREEN);
	// xcb_generic_error_t *err = xcb_request_check(wm->connection, c);
	// if (err) {
	// 	log_message(
	// 		ERROR, "Error removing window property: %d\n", err->error_code);
	// 	free(err);
	// 	return -1;
	// }
out:
	xcb_flush(wm->connection);
	return 0;
}

size_t get_active_clients_size(desktop_t **d, const int n) {
	size_t t = 0;
	for (int i = 0; i < n; ++i) {
		t += d[i]->n_of_clients;
	}
	return t;
}

void populate_client_array(node_t *root, xcb_window_t *arr, size_t *index) {
	if (root == NULL) {
		return;
	}

	if (root->client != NULL && root->client->window != XCB_NONE) {
		arr[*index] = root->client->window;
		(*index)++;
	}

	populate_client_array(root->first_child, arr, index);
	populate_client_array(root->second_child, arr, index);
}

void ewmh_update_client_list() {
	size_t size = get_active_clients_size(wm->desktops, wm->n_of_desktops);
	if (size == 0) {
		xcb_ewmh_set_client_list(wm->ewmh, wm->screen_nbr, 0, NULL);
		return;
	}
	// xcb_window_t active_clients[size + 1];
	xcb_window_t *active_clients =
		(xcb_window_t *)malloc((size + 1) * sizeof(xcb_window_t));
	if (active_clients == NULL) {
		return;
	}
	// TODO: traverse trees and add window ids
	//  to and array of type xcb_window_t
	//  xcb_ewmh_set_client_list(...)
	// Traverse all desktop trees and add window IDs to active_clients array
	size_t index = 0;
	for (int i = 0; i < wm->n_of_desktops; ++i) {
		node_t *root = wm->desktops[i]->tree;
		populate_client_array(root, active_clients, &index);
	}
	xcb_ewmh_set_client_list(wm->ewmh, wm->screen_nbr, size, active_clients);
	free(active_clients);
	active_clients = NULL;
}

int ewmh_update_current_desktop(xcb_ewmh_connection_t *ewmh, int screen_nbr,
								uint32_t i) {
	xcb_void_cookie_t c =
		xcb_ewmh_set_current_desktop_checked(ewmh, screen_nbr, i);
	xcb_generic_error_t *err = xcb_request_check(ewmh->connection, c);
	if (err) {
		log_message(
			ERROR, "Error setting number of desktops: %d\n", err->error_code);
		free(err);
		return -1;
	}
	return 0;
}

xcb_get_geometry_reply_t *get_geometry(xcb_window_t win, xcb_conn_t *conn) {
	xcb_get_geometry_cookie_t gc = xcb_get_geometry_unchecked(conn, win);
	xcb_generic_error_t		 *err;
	xcb_get_geometry_reply_t *gr = xcb_get_geometry_reply(conn, gc, &err);

	if (err) {
		log_message(ERROR,
					"Error getting geometry for window %u: %d\n",
					win,
					err->error_code);
		free(err);
		return NULL;
	}

	if (gr == NULL) {
		log_message(ERROR, "Failed to get geometry for window %u\n", win);
		return NULL;
	}

	return gr;
}

client_t *create_client(xcb_window_t win, xcb_atom_t wtype, xcb_conn_t *conn) {
	client_t *c = (client_t *)malloc(sizeof(client_t));
	if (c == 0x00) {
		return NULL;
	}
	snprintf(c->name, sizeof(c->name), "%s", NULL_STR);
	c->id					   = win;
	c->is_managed			   = false;
	c->window				   = win;
	c->type					   = wtype;
	c->border_width			   = (uint32_t)-1;
	const uint32_t	  mask	   = XCB_CW_EVENT_MASK;
	const uint32_t	  values[] = {CLIENT_EVENT_MASK};
	xcb_void_cookie_t cookie =
		xcb_change_window_attributes_checked(conn, c->window, mask, values);
	xcb_generic_error_t *err = xcb_request_check(conn, cookie);

	if (err) {
		log_message(ERROR,
					"Error setting window attributes for client %u: %d\n",
					c->window,
					err->error_code);
		free(err);
		exit(EXIT_FAILURE);
	}

	return c;
}

desktop_t *init_desktop() {
	desktop_t *d = (desktop_t *)malloc(sizeof(desktop_t));
	if (d == 0x00) {
		return NULL;
	}
	d->id			= 0;
	d->is_focused	= false;
	d->n_of_clients = 0;
	d->tree			= NULL;
	return d;
}

wm_t *initialize_wm() {
	int i, default_screen;
	wm = (wm_t *)malloc(sizeof(wm_t));
	if (wm == NULL) {
		log_message(ERROR, "Failed to malloc for window manager\n");
		return NULL;
	}

	wm->connection = xcb_connect(NULL, &default_screen);
	if (xcb_connection_has_error(wm->connection) > 0) {
		log_message(ERROR, "Error: Unable to open X connection\n");
		free(wm);
		return NULL;
	}

	const xcb_setup_t	 *setup = xcb_get_setup(wm->connection);
	xcb_screen_iterator_t iter	= xcb_setup_roots_iterator(setup);
	for (i = 0; i < default_screen; ++i) {
		xcb_screen_next(&iter);
	}

	desktop_t **desktops =
		(desktop_t **)malloc(sizeof(desktop_t *) * NUMBER_OF_DESKTOPS);
	if (desktops == NULL) {
		log_message(ERROR, "Failed to malloc desktops\n");
		free(wm);
		return NULL;
	}

	wm->screen				   = iter.data;
	wm->root_window			   = iter.data->root;
	wm->screen_nbr			   = default_screen;
	wm->desktops			   = desktops;
	wm->n_of_desktops		   = NUMBER_OF_DESKTOPS;
	wm->split_type			   = DYNAMIC_TYPE;
	const uint32_t	  mask	   = XCB_CW_EVENT_MASK;
	const uint32_t	  values[] = {ROOT_EVENT_MASK};

	xcb_void_cookie_t cookie   = xcb_change_window_attributes_checked(
		  wm->connection, wm->root_window, mask, values);
	xcb_generic_error_t *err = xcb_request_check(wm->connection, cookie);
	if (err) {
		log_message(
			ERROR,
			"Error registering for substructure redirection events on window "
			"%u: %d\n",
			wm->root_window,
			err->error_code);
		free(wm);
		free(err);
		return NULL;
	}

	for (int j = 0; j < NUMBER_OF_DESKTOPS; j++) {
		desktop_t *d  = init_desktop();
		d->id		  = (uint8_t)j;
		d->is_focused = j == 0 ? true : false;
		d->layout	  = DEFAULT;
		snprintf(d->name, sizeof(d->name), "%d", j + 1);
		wm->desktops[j] = d;
	}

	return wm;
}

bool setup_ewmh() {
	wm->ewmh = ewmh_init(wm->connection);
	if (wm->ewmh == NULL) {
		return false;
	}

	xcb_atom_t		  net_atoms[] = {wm->ewmh->_NET_SUPPORTED,
									 wm->ewmh->_NET_SUPPORTING_WM_CHECK,
									 wm->ewmh->_NET_DESKTOP_NAMES,
									 wm->ewmh->_NET_DESKTOP_VIEWPORT,
									 wm->ewmh->_NET_NUMBER_OF_DESKTOPS,
									 wm->ewmh->_NET_CURRENT_DESKTOP,
									 wm->ewmh->_NET_CLIENT_LIST,
									 wm->ewmh->_NET_ACTIVE_WINDOW,
									 wm->ewmh->_NET_WM_NAME,
									 wm->ewmh->_NET_CLOSE_WINDOW,
									 wm->ewmh->_NET_WM_STRUT_PARTIAL,
									 wm->ewmh->_NET_WM_DESKTOP,
									 wm->ewmh->_NET_WM_STATE,
									 wm->ewmh->_NET_WM_STATE_HIDDEN,
									 wm->ewmh->_NET_WM_STATE_FULLSCREEN,
									 wm->ewmh->_NET_WM_STATE_BELOW,
									 wm->ewmh->_NET_WM_STATE_ABOVE,
									 wm->ewmh->_NET_WM_STATE_STICKY,
									 wm->ewmh->_NET_WM_STATE_DEMANDS_ATTENTION,
									 wm->ewmh->_NET_WM_WINDOW_TYPE,
									 wm->ewmh->_NET_WM_WINDOW_TYPE_DOCK,
									 wm->ewmh->_NET_WM_WINDOW_TYPE_DESKTOP,
									 wm->ewmh->_NET_WM_WINDOW_TYPE_NOTIFICATION,
									 wm->ewmh->_NET_WM_WINDOW_TYPE_DIALOG,
									 wm->ewmh->_NET_WM_WINDOW_TYPE_SPLASH,
									 wm->ewmh->_NET_WM_WINDOW_TYPE_UTILITY,
									 wm->ewmh->_NET_WM_WINDOW_TYPE_TOOLBAR};

	xcb_void_cookie_t c			  = xcb_ewmh_set_supported_checked(
		  wm->ewmh, wm->screen_nbr, LENGTH(net_atoms), net_atoms);
	xcb_generic_error_t *err = xcb_request_check(wm->ewmh->connection, c);
	if (err) {
		log_message(
			ERROR, "Error setting number of desktops: %d\n", err->error_code);
		free(err);
		return false;
	}

	// uint32_t meta_window = xcb_generate_id(wm->connection);
	// xcb_create_window(
	//     wm->connection,
	//     XCB_COPY_FROM_PARENT,
	//     meta_window,
	//     wm->root_window,
	//     -1,
	//     -1,
	//     1,
	//     1,
	//     0,
	//     XCB_WINDOW_CLASS_INPUT_ONLY,
	//     XCB_COPY_FROM_PARENT,
	//     XCB_NONE,
	//     NULL
	// );
	if (ewmh_set_supporting(wm->root_window, wm->ewmh) != 0) {
		return false;
	}

	if (ewmh_set_number_of_desktops(
			wm->ewmh, wm->screen_nbr, (uint32_t)wm->n_of_desktops) != 0) {
		return false;
	}

	const int di = get_focused_desktop_idx();
	if (di == -1) {
		return false;
	}

	if (ewmh_update_current_desktop(wm->ewmh, wm->screen_nbr, (uint32_t)di) !=
		0) {
		return false;
	}

	if (ewmh_update_desktop_names() != 0) {
		return false;
	}

	return true;
}

wm_t *init_wm() {
	wm = initialize_wm();
	if (wm == NULL) {
		return NULL;
	}

	if (!setup_ewmh()) {
		return NULL;
	}

	return wm;
}

int resize_window(xcb_window_t win, uint16_t width, uint16_t height) {
	if (win == 0 || win == XCB_NONE) return 0;

	const uint32_t	  values[] = {width, height};
	xcb_void_cookie_t cookie =
		xcb_configure_window_checked(wm->connection, win, XCB_RESIZE, values);
	xcb_generic_error_t *err = xcb_request_check(wm->connection, cookie);

	if (err) {
		log_message(
			ERROR, "Error resizing window (ID %u): %d", win, err->error_code);
		free(err);
		return -1;
	}

	return 0;
}

int move_window(xcb_window_t win, int16_t x, int16_t y) {
	if (win == 0 || win == XCB_NONE) {
		return 0;
	}

	const uint32_t	  values[] = {x, y};
	xcb_void_cookie_t cookie =
		xcb_configure_window_checked(wm->connection, win, XCB_MOVE, values);
	xcb_generic_error_t *err = xcb_request_check(wm->connection, cookie);

	if (err) {
		log_message(
			ERROR, "Error moving window (ID %u): %d", win, err->error_code);
		free(err);
		return -1;
	}

	return 0;
}

int change_border_attr(xcb_conn_t *conn, xcb_window_t win, uint32_t bcolor,
					   uint32_t bwidth, bool stack) {

	uint32_t bpx_width = XCB_CW_BORDER_PIXEL;
	uint32_t b_width   = XCB_CONFIG_WINDOW_BORDER_WIDTH;
	uint32_t stack_	   = XCB_CONFIG_WINDOW_STACK_MODE;
	uint32_t input	   = XCB_INPUT_FOCUS_PARENT;

	if (change_window_attr(conn, win, bpx_width, &bcolor) != 0) {
		return -1;
	}

	if (configure_window(conn, win, b_width, &bwidth) != 0) {
		return -1;
	}

	if (stack) {
		const uint16_t arg[1] = {XCB_STACK_MODE_ABOVE};
		if (configure_window(conn, win, stack_, arg) != 0) {
			return -1;
		}
		if (set_input_focus(conn, input, win, XCB_CURRENT_TIME) != 0) {
			return -1;
		}
		// goto out;
	}

	// if (set_input_focus(conn, input, XCB_NONE, XCB_CURRENT_TIME) != 0) {
	// 	return -1;
	// }

	// out:
	xcb_flush(conn);
	return 0;
}

int change_window_attr(xcb_conn_t *conn, xcb_window_t win, uint32_t attr,
					   const void *val) {
	xcb_void_cookie_t attr_cookie =
		xcb_change_window_attributes_checked(conn, win, attr, val);
	xcb_generic_error_t *err = xcb_request_check(conn, attr_cookie);
	if (err != NULL) {
		log_message(ERROR,
					"Failed to change window attributes: error code %d",
					err->error_code);
		free(err);
		return -1;
	}
	return 0;
}

int configure_window(xcb_conn_t *conn, xcb_window_t win, uint16_t attr,
					 const void *val) {
	xcb_void_cookie_t config_cookie =
		xcb_configure_window_checked(conn, win, attr, val);
	xcb_generic_error_t *err = xcb_request_check(conn, config_cookie);
	if (err != NULL) {
		log_message(ERROR,
					"Failed to configure window : error code %d",
					err->error_code);
		free(err);
		return -1;
	}
	return 0;
}

int set_input_focus(xcb_conn_t *conn, uint8_t revert_to, xcb_window_t win,
					xcb_timestamp_t time) {
	xcb_void_cookie_t focus_cookie =
		xcb_set_input_focus_checked(conn, revert_to, win, time);
	xcb_generic_error_t *err = xcb_request_check(conn, focus_cookie);

	if (err != NULL) {
		log_message(ERROR,
					"Failed to set input focus : error code %d",
					err->error_code);
		free(err);
		return -1;
	}
	return 0;
}

int tile(node_t *node) {
	if (node == NULL || node->client == NULL) {
		return -1;
	}

	const uint16_t width  = node->rectangle.width;
	const uint16_t height = node->rectangle.height;
	const int16_t  x	  = node->rectangle.x;
	const int16_t  y	  = node->rectangle.y;

	if (resize_window(node->client->window, width, height) != 0 ||
		move_window(node->client->window, x, y) != 0) {
		return -1;
	}

	xcb_void_cookie_t cookie =
		xcb_map_window_checked(wm->connection, node->client->window);
	xcb_generic_error_t *err = xcb_request_check(wm->connection, cookie);
	if (err != NULL) {
		log_message(ERROR,
					"Errror in mapping window %d: error code %d",
					node->client->window,
					err->error_code);
		free(err);
		return -1;
	}

	xcb_flush(wm->connection);
	return 0;
}

bool supports_protocol(xcb_window_t win, xcb_atom_t atom, xcb_conn_t *conn) {
	xcb_get_property_cookie_t		   cookie = {0};
	xcb_icccm_get_wm_protocols_reply_t protocols;
	bool							   result = false;
	xcb_atom_t WM_PROTOCOLS					  = get_atom("WM_PROTOCOLS", conn);

	cookie = xcb_icccm_get_wm_protocols(conn, win, WM_PROTOCOLS);
	if (xcb_icccm_get_wm_protocols_reply(conn, cookie, &protocols, NULL) != 1) {
		return false;
	}

	for (uint32_t i = 0; i < protocols.atoms_len; i++) {
		if (protocols.atoms[i] == atom) {
			result = true;
		}
	}

	xcb_icccm_get_wm_protocols_reply_wipe(&protocols);

	return result;
}

int display_bar(rectangle_t r, xcb_window_t win) {
	uint16_t width	= r.width;
	uint16_t height = r.height;
	int16_t	 x		= r.x;
	int16_t	 y		= r.y;

	if (resize_window(win, width, height) != 0 || move_window(win, x, y) != 0) {
		return -1;
	}

	xcb_void_cookie_t	 cookie = xcb_map_window_checked(wm->connection, win);
	xcb_generic_error_t *err	= xcb_request_check(wm->connection, cookie);
	if (err != NULL) {
		log_message(ERROR,
					"Errror in mapping window %d: error code %d",
					win,
					err->error_code);
		free(err);
		return -1;
	}

	xcb_flush(wm->connection);
	return 0;
}

int16_t get_cursor_axis(xcb_conn_t *conn, xcb_window_t win) {
	xcb_query_pointer_cookie_t p_cookie = xcb_query_pointer(conn, win);
	xcb_query_pointer_reply_t *p_reply =
		xcb_query_pointer_reply(conn, p_cookie, NULL);

	if (p_reply == NULL) {
		log_message(ERROR, "Failed to query pointer position\n");
		return -1;
	}

	int16_t x = p_reply->root_x;
	free(p_reply);

	return x;
}

xcb_window_t get_window_under_cursor(xcb_conn_t *conn, xcb_window_t win) {
	xcb_query_pointer_cookie_t p_cookie = xcb_query_pointer(conn, win);
	xcb_query_pointer_reply_t *p_reply =
		xcb_query_pointer_reply(conn, p_cookie, NULL);

	if (p_reply == NULL) {
		log_message(ERROR, "Failed to query pointer position\n");
		return XCB_NONE;
	}

	xcb_window_t x = p_reply->child;
	free(p_reply);

	return x;
}

xcb_keycode_t *get_keycode(xcb_keysym_t keysym, xcb_conn_t *conn) {
	xcb_key_symbols_t *keysyms = NULL;
	xcb_keycode_t	  *keycode = NULL;

	if ((keysyms = xcb_key_symbols_alloc(conn)) == NULL) {
		xcb_key_symbols_free(keysyms);
		return NULL;
	}

	keycode = xcb_key_symbols_get_keycode(keysyms, keysym);
	xcb_key_symbols_free(keysyms);

	return keycode;
}

xcb_keysym_t get_keysym(xcb_keycode_t keycode, xcb_conn_t *conn) {
	xcb_key_symbols_t *keysyms = NULL;
	xcb_keysym_t	   keysym;

	if ((keysyms = xcb_key_symbols_alloc(conn)) == NULL) {
		keysym = 0;
	}

	keysym = xcb_key_symbols_get_keysym(keysyms, keycode, 0);
	xcb_key_symbols_free(keysyms);

	return keysym;
}

int grab_keys(xcb_conn_t *conn, xcb_window_t win) {
	if (conn == NULL || win == XCB_NONE) {
		return -1;
	}

	const size_t n = sizeof(keys_) / sizeof(keys_[0]);

	for (size_t i = 0; i < n; ++i) {
		xcb_keycode_t *key = get_keycode(keys_[i].keysym, conn);
		if (key == NULL) return -1;
		xcb_void_cookie_t cookie = xcb_grab_key_checked(conn,
														1,
														win,
														(uint16_t)keys_[i].mod,
														*key,
														XCB_GRAB_MODE_ASYNC,
														XCB_GRAB_MODE_ASYNC);
		free(key);
		xcb_generic_error_t *err = xcb_request_check(conn, cookie);
		if (err != NULL) {
			log_message(ERROR, "error grabbing key %d\n", err->error_code);
			free(err);
			return -1;
		}
	}

	return 0;
}

xcb_atom_t get_atom(char *atom_name, xcb_conn_t *conn) {
	xcb_intern_atom_cookie_t atom_cookie;
	xcb_atom_t				 atom;
	xcb_intern_atom_reply_t *rep;

	atom_cookie =
		xcb_intern_atom(conn, 0, (uint16_t)strlen(atom_name), atom_name);
	rep = xcb_intern_atom_reply(conn, atom_cookie, NULL);
	if (NULL != rep) {
		atom = rep->atom;
		free(rep);
		return atom;
	}
	return 0;
}

int send_client_message(xcb_window_t win, xcb_atom_t property, xcb_atom_t value,
						xcb_conn_t *conn) {
	xcb_client_message_event_t *e = calloc(32, 1);
	e->response_type			  = XCB_CLIENT_MESSAGE;
	e->window					  = win;
	e->type						  = property;
	e->format					  = 32;
	e->data.data32[0]			  = value;
	e->data.data32[1]			  = XCB_CURRENT_TIME;
	xcb_void_cookie_t c			  = xcb_send_event_checked(
		  conn, false, win, XCB_EVENT_MASK_NO_EVENT, (char *)e);
	xcb_generic_error_t *err = xcb_request_check(conn, c);
	if (err != NULL) {
		log_message(ERROR, "error sending event: %d\n", err->error_code);
		free(e);
		free(err);
		return -1;
	}
	xcb_flush(conn);
	free(e);
	return 0;
}

int close_or_kill(arg_t *arg) {
	xcb_window_t win = get_window_under_cursor(wm->connection, wm->root_window);
	if (!window_exists(wm->connection, win)) return 0;

	xcb_atom_t wm_delete = get_atom("WM_DELETE_WINDOW", wm->connection);
	xcb_icccm_get_text_property_reply_t t_reply;
	xcb_get_property_cookie_t cn = xcb_icccm_get_wm_name(wm->connection, win);
	const uint8_t			  wr =
		xcb_icccm_get_wm_name_reply(wm->connection, cn, &t_reply, NULL);
	if (supports_protocol(win, wm_delete, wm->connection)) {
		if (wr == 1) {
			log_message(
				DEBUG,
				"window id = %d, reply name = %s: supports WM_DELETE_WINDOW\n",
				win,
				t_reply.name);
			xcb_icccm_get_text_property_reply_wipe(&t_reply);
		}
		int ret = send_client_message(
			win, wm->ewmh->WM_PROTOCOLS, wm_delete, wm->connection);
		if (ret != 0) {
			log_message(ERROR, "failed to send client message\n");
			return -1;
		}
		return 0;
	}

	xcb_void_cookie_t	 c	 = xcb_kill_client_checked(wm->connection, win);
	xcb_generic_error_t *err = xcb_request_check(wm->connection, c);
	if (err != NULL) {
		log_message(ERROR,
					"error closing window: %d, error: %d\n",
					win,
					err->error_code);
		free(err);
		return -1;
	}
	return 0;
}

void ungrab_keys(xcb_conn_t *conn, xcb_window_t win) {
	if (conn == NULL || win == XCB_NONE) {
		return;
	}

	const xcb_keycode_t modifier = (xcb_keycode_t)XCB_MOD_MASK_ANY;
	xcb_void_cookie_t	cookie =
		xcb_ungrab_key_checked(conn, XCB_GRAB_ANY, win, modifier);
	xcb_generic_error_t *err = xcb_request_check(conn, cookie);
	if (err != NULL) {
		log_message(ERROR, "error ungrabbing keys: %d\n", err->error_code);
		free(err);
	}
}

void map_floating(xcb_window_t x) {
	rectangle_t				  rc = {0};
	xcb_get_geometry_reply_t *g	 = get_geometry(x, wm->connection);
	if (g == NULL) {
		return;
	}
	rc.height = g->height;
	rc.width  = g->width;
	rc.x	  = g->x;
	rc.y	  = g->y;
	free(g);
	resize_window(x, rc.width, rc.height);
	move_window(x, rc.x, rc.y);
	xcb_map_window(wm->connection, x);
}

int kill_window(xcb_window_t win) {
	if (win == XCB_NONE) {
		return -1;
	}

	xcb_icccm_get_text_property_reply_t t_reply;
	xcb_get_property_cookie_t cn = xcb_icccm_get_wm_name(wm->connection, win);
	const uint8_t			  wr =
		xcb_icccm_get_wm_name_reply(wm->connection, cn, &t_reply, NULL);
	if (wr == 1) {
		log_message(DEBUG,
					"delete window id = %d, reply name = %s\n",
					win,
					t_reply.name);
		xcb_icccm_get_text_property_reply_wipe(&t_reply);
	}

	if (win == 0) {
		return 0;
	}

	int curi = get_focused_desktop_idx();
	if (curi == -1) {
		log_message(ERROR, "cannot find focused desktop");
		return curi;
	}

	node_t *n = find_node_by_window_id(wm->desktops[curi]->tree, win);

	if (n == NULL) {
		log_message(ERROR, "cannot fond node with window %d", win);
		return -1;
	}

	xcb_void_cookie_t cookie =
		xcb_unmap_window(wm->connection, n->client->window);
	xcb_generic_error_t *err = xcb_request_check(wm->connection, cookie);
	if (err != NULL) {
		log_message(ERROR,
					"Errror in unmapping window %d: error code %d",
					n->client->window,
					err->error_code);
		free(err);

		log_message(INFO, "err != null");
		return -1;
	}

	desktop_t *d = wm->desktops[curi];
	delete_node(n, d);
	//	log_tree_nodes(d->tree, wm);
	ewmh_update_client_list();

	if (is_tree_empty(d->tree)) {
		set_active_window_name(XCB_NONE);
	}

	if (render_tree(d->tree) != 0) {
		log_message(ERROR, "cannot render tree");
		return -1;
	}

	return 0;
}

int show_window(xcb_window_t win) {
	xcb_generic_error_t *err;
	xcb_void_cookie_t	 c;
	/* According to ewmh:
	 * Mapped windows should be placed in NormalState, according to the ICCCM.
	 **/
	c	= xcb_map_window_checked(wm->connection, win);
	err = xcb_request_check(wm->connection, c);
	if (err != NULL) {
		log_message(ERROR,
					"Cannot hide window %d: error code %d",
					win,
					err->error_code);
		free(err);
		return -1;
	}

	// set window property to NormalState
	// XCB_ICCCM_WM_STATE_NORMAL
	const long		 data[] = {XCB_ICCCM_WM_STATE_NORMAL, XCB_NONE};
	const xcb_atom_t wm_s	= get_atom("WM_STATE", wm->connection);
	c						= xcb_change_property_checked(
		  wm->connection, XCB_PROP_MODE_REPLACE, win, wm_s, wm_s, 32, 2, data);
	err = xcb_request_check(wm->connection, c);
	if (err != NULL) {
		log_message(ERROR,
					"Cannot change window property %d: error code %d",
					win,
					err->error_code);
		free(err);
		return -1;
	}
	return 0;
}

int hide_window(xcb_window_t win) {
	xcb_generic_error_t *err;
	xcb_void_cookie_t	 c;
	/* According to ewmh:
	 * Unmapped windows should be placed in IconicState, according to the ICCCM.
	 * Windows which are actually iconified or minimized should have the
	 *_NET_WM_STATE_HIDDEN property set, to communicate to pagers that the
	 *window should not be represented as "onscreen."
	 **/
	c	= xcb_unmap_window_checked(wm->connection, win);
	err = xcb_request_check(wm->connection, c);
	if (err != NULL) {
		log_message(ERROR,
					"Cannot hide window %d: error code %d",
					win,
					err->error_code);
		free(err);
		return -1;
	}

	// set window property to IconicState
	// XCB_ICCCM_WM_STATE_ICONIC
	const long		 data[] = {XCB_ICCCM_WM_STATE_ICONIC, XCB_NONE};
	const xcb_atom_t wm_s	= get_atom("WM_STATE", wm->connection);
	c						= xcb_change_property_checked(
		  wm->connection, XCB_PROP_MODE_REPLACE, win, wm_s, wm_s, 32, 2, data);
	err = xcb_request_check(wm->connection, c);
	if (err != NULL) {
		log_message(ERROR,
					"Cannot change window property %d: error code %d",
					win,
					err->error_code);
		free(err);
		return -1;
	}
	if (change_border_attr(wm->connection, win, 0x000000, 0, false) != 0) {
		log_message(ERROR, "Failed to change border attr for window %d\n", win);
		return -1;
	}

	// lower_window(wm->connection, win);

	return 0;
}

int exec_process(arg_t *arg) {
	pid_t pid = fork();
	if (pid < 0) {
		perror("Fork failed");
		exit(EXIT_FAILURE);
	} else if (pid == 0) {
		const char *args[] = {arg->cmd, NULL};
		execvp(arg->cmd, (char *const *)args);
		perror("execvp failed");
		exit(EXIT_FAILURE);
	}
	return 0;
}

void rofi_exec(char *dir) {
	pid_t pid = fork();
	if (pid == -1) {
		perror("fork");
		exit(EXIT_FAILURE);
	} else if (pid == 0) {
		char *args[] = {(char *const)"rofi", "-show", (char *)dir, NULL};
		execvp("rofi", args);
		perror("execvp");
		exit(EXIT_FAILURE);
	} else {
		wait(NULL);
	}
}

void shift_cmd(xcb_key_press_event_t *key_event, wm_t *wm) {
	xcb_keysym_t k = get_keysym(key_event->detail, wm->connection);
	xcb_window_t w = get_window_under_cursor(wm->connection, wm->root_window);
	if (w == wm->root_window) {
		return;
	}
	int d = get_focused_desktop_idx();
	if (d == -1) {
		return;
	}
	node_t *root = wm->desktops[d]->tree;
	node_t *node = find_node_by_window_id(root, w);

	for (int i = 1; i <= wm->n_of_desktops; ++i) {
		if (k == (xcb_keysym_t)(XK_0 + i)) {
			if (d == i - 1) {
				return;
			}
			desktop_t *nd = wm->desktops[i - 1];
			desktop_t *od = wm->desktops[d];
			if (hide_window(node->client->window) != 0) {
				return;
			}
			unlink_node(node, od);
			transfer_node(node, nd);
			if (render_tree(od->tree) != 0) {
				return;
			}
			break;
		}
	}
}

void polybar_exec(char *dir) {
	pid_t pid = fork();

	if (pid == -1) {
		perror("fork");
		exit(EXIT_FAILURE);
	} else if (pid == 0) {
		char *args[] = {"polybar", "-c", (char *)dir, NULL};
		execvp("polybar", args);
		perror("execvp");
		exit(EXIT_FAILURE);
	} else {
		wait(NULL);
	}
}

void update_focused_desktop(int id) {
	for (int i = 0; i < wm->n_of_desktops; ++i) {
		if (wm->desktops[i]->id != id) {
			wm->desktops[i]->is_focused = false;
		} else {
			wm->desktops[i]->is_focused = true;
		}
	}
}

int switch_desktop_wrapper(arg_t *arg) {
	if (switch_desktop(arg->idx) != 0) {
		return -1;
	}
	return render_tree(wm->desktops[arg->idx]->tree);
}

int switch_desktop(const int nd) {
	int current = get_focused_desktop_idx();
	if (current == -1) {
		return current;
	}

	if (ewmh_update_current_desktop(wm->ewmh, wm->screen_nbr, nd) != 0) {
		return -1;
	}

	update_focused_desktop(nd);
	const uint32_t		 _off[] = {ROOT_EVENT_MASK &
								   ~XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY};
	const uint32_t		 _on[]	= {ROOT_EVENT_MASK};
	xcb_generic_error_t *err;
	xcb_void_cookie_t	 c;
	c = xcb_change_window_attributes_checked(
		wm->connection, wm->root_window, XCB_CW_EVENT_MASK, _off);
	err = xcb_request_check(wm->connection, c);
	if (err != NULL) {
		log_message(ERROR,
					"Cannot change root window %d attrs: error code %d",
					wm->root_window,
					err->error_code);
		free(err);
		return -1;
	}

	if (hide_windows(wm->desktops[current]->tree) != 0) {
		// TODO: handle error
		return -1;
	}
	if (show_windows(wm->desktops[nd]->tree) != 0) {
		// TODO: handle error
		return -1;
	}
	c = xcb_change_window_attributes_checked(
		wm->connection, wm->root_window, XCB_CW_EVENT_MASK, _on);
	err = xcb_request_check(wm->connection, c);
	if (err != NULL) {
		log_message(ERROR,
					"Cannot change root window %d attrs: error code %d",
					wm->root_window,
					err->error_code);
		free(err);
		return -1;
	}
	xcb_flush(wm->connection);
	log_message(INFO, "new desktop %d nodes--------------", nd + 1);
	log_tree_nodes(wm->desktops[nd]->tree);
	log_message(INFO, "old desktop %d nodes--------------", current + 1);
	log_tree_nodes(wm->desktops[current]->tree);
	return 0;
}

int set_active_window_name(xcb_window_t win) {
	xcb_void_cookie_t aw_cookie =
		xcb_ewmh_set_active_window_checked(wm->ewmh, wm->screen_nbr, win);
	xcb_generic_error_t *err = xcb_request_check(wm->connection, aw_cookie);
	if (err) {
		log_message(
			ERROR, "Error setting active window: %d\n", err->error_code);
		free(err);
		return -1;
	}

	return 0;
}

int set_window_state(xcb_window_t win, xcb_icccm_wm_state_t state) {
	const long		  data[] = {state, XCB_NONE};
	xcb_atom_t		  t		 = get_atom("WM_STATE", wm->connection);
	xcb_void_cookie_t c		 = xcb_change_property_checked(
		 wm->connection, XCB_PROP_MODE_REPLACE, win, t, t, 32, 2, data);
	xcb_generic_error_t *err = xcb_request_check(wm->connection, c);
	if (err != NULL) {
		log_message(ERROR,
					"Errror in changing property window %d: error code %d",
					win,
					err->error_code);
		free(err);
		return -1;
	}
	return 0;
}

bool should_manage(xcb_window_t win, xcb_conn_t *conn) {
	xcb_get_window_attributes_cookie_t attr_cookie;
	xcb_get_window_attributes_reply_t *attr_reply;

	attr_cookie = xcb_get_window_attributes(conn, win);
	attr_reply	= xcb_get_window_attributes_reply(conn, attr_cookie, NULL);

	if (attr_reply == NULL) {
		return true;
	}

	bool manage = !attr_reply->override_redirect;
	free(attr_reply);
	return manage;
}

int window_type(xcb_window_t win) {
	xcb_ewmh_get_atoms_reply_t w_type;
	xcb_get_property_cookie_t  c = xcb_ewmh_get_wm_window_type(wm->ewmh, win);
	const uint8_t			   r =
		xcb_ewmh_get_wm_window_type_reply(wm->ewmh, c, &w_type, NULL);
	if (r == 1) {
		for (unsigned int i = 0; i < w_type.atoms_len; i++) {
			const xcb_atom_t a = w_type.atoms[i];
			if (a == wm->ewmh->_NET_WM_WINDOW_TYPE_NORMAL) {
				/*
				 * _NET_WM_WINDOW_TYPE_NORMAL
				 * indicates that this is a normal, top-level window.
				 * */
				xcb_ewmh_get_atoms_reply_wipe(&w_type);
				return 1;
			} else if (a == wm->ewmh->_NET_WM_WINDOW_TYPE_DOCK) {
				/*
				 * _NET_WM_WINDOW_TYPE_DOCK
				 * indicates a dock or panel feature.
				 * Typically a Window Manager would keep such windows
				 * on top of all other windows.
				 * */
				xcb_ewmh_get_atoms_reply_wipe(&w_type);
				return 2;
			} else if (a == wm->ewmh->_NET_WM_WINDOW_TYPE_TOOLBAR ||
					   a == wm->ewmh->_NET_WM_WINDOW_TYPE_MENU) {
				/*
				 * _NET_WM_WINDOW_TYPE_TOOLBAR and _NET_WM_WINDOW_TYPE_MENU
				 * indicate toolbar and pinnable menu windows,
				 * respectively (i.e. toolbars and menus "torn off" from the
				 * main application). Windows of this type may set the
				 * WM_TRANSIENT_FOR hint indicating the main application window.
				 * */
				xcb_ewmh_get_atoms_reply_wipe(&w_type);
				return 3;
			} else if (a == wm->ewmh->_NET_WM_WINDOW_TYPE_UTILITY) {
				/*
				 * _NET_WM_WINDOW_TYPE_UTILITY
				 * indicates a small persistent utility window, such as a
				 * palette or toolbox. It is distinct from type TOOLBAR because
				 * it does not correspond to a toolbar torn off from the main
				 * application. It's distinct from type DIALOG because it isn't
				 * a transient dialog, the user will probably keep it open while
				 * they're working. Windows of this type may set the
				 * WM_TRANSIENT_FOR hint indicating the main application window.
				 * */
				xcb_ewmh_get_atoms_reply_wipe(&w_type);
				return 4;
			} else if (a == wm->ewmh->_NET_WM_WINDOW_TYPE_SPLASH) {
				/*
				 * _NET_WM_WINDOW_TYPE_SPLASH
				 * indicates that the window is a splash screen
				 * displayed as an application is starting up.
				 * */
				xcb_ewmh_get_atoms_reply_wipe(&w_type);
				return 5;
			} else if (a == wm->ewmh->_NET_WM_WINDOW_TYPE_DIALOG ||
					   a != wm->ewmh->_NET_WM_WINDOW_TYPE) {
				/*
				 * _NET_WM_WINDOW_TYPE_DIALOG
				 * indicates that this is a dialog window.
				 * If _NET_WM_WINDOW_TYPE is not set,
				 * then windows with WM_TRANSIENT_FOR set MUST be taken as this
				 * type.
				 * */
				xcb_ewmh_get_atoms_reply_wipe(&w_type);
				return 6;
			} else {
				xcb_ewmh_get_atoms_reply_wipe(&w_type);
				return 1;
			}
		}
	}
	return -1;
}

bool is_splash(xcb_window_t win) {
	xcb_icccm_get_text_property_reply_t t_reply;
	xcb_get_property_cookie_t cn = xcb_icccm_get_wm_name(wm->connection, win);
	const uint8_t			  wr =
		xcb_icccm_get_wm_name_reply(wm->connection, cn, &t_reply, NULL);
	if (wr == 1) {
		if (strcmp(t_reply.name, "splash") == 0) {
			xcb_icccm_get_text_property_reply_wipe(&t_reply);
			return true;
		}
	}
	return false;
}

bool window_exists(xcb_conn_t *conn, xcb_window_t win) {
	xcb_query_tree_cookie_t c		   = xcb_query_tree(conn, win);
	xcb_query_tree_reply_t *tree_reply = xcb_query_tree_reply(conn, c, NULL);

	if (tree_reply == NULL) {
		return false;
	}

	free(tree_reply);
	return true;
}

bool is_transient(xcb_window_t win) {
	xcb_window_t			  transient = XCB_NONE;
	xcb_get_property_cookie_t c =
		xcb_icccm_get_wm_transient_for(wm->connection, win);
	const uint8_t r = xcb_icccm_get_wm_transient_for_reply(
		wm->connection, c, &transient, NULL);

	if (r != 1) {
		return false;
	}

	if (transient != XCB_NONE) {
		return true;
	}
	return false;
}

int handle_first_window(client_t *client, desktop_t *d) {
	rectangle_t	   r = {0};
	const uint16_t w = wm->screen->width_in_pixels;
	const uint16_t h = wm->screen->height_in_pixels;
	if (XCB_NONE != wbar) {
		xcb_get_geometry_reply_t *g = get_geometry(wbar, wm->connection);
		r.x							= W_GAP;
		r.y							= (int16_t)(g->height + W_GAP);
		r.width						= (uint16_t)(w - 2 * W_GAP);
		r.height					= (uint16_t)(h - 2 * W_GAP - g->height);
		free(g);
		g = NULL;
	} else {
		r.x		 = W_GAP;
		r.y		 = W_GAP;
		r.width	 = w - W_GAP;
		r.height = h - W_GAP;
	}

	if (client == NULL) {
		log_message(ERROR, "client is null");
		return -1;
	}

	d->tree			   = init_root();
	d->tree->client	   = client;
	d->tree->rectangle = r;
	d->n_of_clients += 1;

	log_tree_nodes(d->tree);
	ewmh_update_client_list();
	return tile(d->tree);
}

int handle_subsequent_window(client_t *client, desktop_t *d) {
	xcb_window_t wi = get_window_under_cursor(wm->connection, wm->root_window);

	if (wi == wm->root_window || wi == 0) {
		// don't attempt to split root_window
		return 0;
	}

	node_t *n = find_node_by_window_id(d->tree, wi);

	if (n == NULL || n->client == NULL) {
		log_message(ERROR, "cannot find node with window id %d", wi);
		return -1;
	}

	if (client == NULL) {
		log_message(ERROR, "client is null");
		return -1;
	}
	log_message(DEBUG, "supsequent request node found");
	node_t *new_node = create_node(client);
	if (new_node == NULL) {
		log_message(ERROR, "new node is null");
		return -1;
	}
	insert_node(n, new_node);
	d->n_of_clients += 1;
	// log_tree_nodes(d->tree, wm);
	ewmh_update_client_list();
	return render_tree(d->tree);
}

int handle_map_request(xcb_window_t win) {
	/*
	 * https://youtrack.jetbrains.com/issue/IDEA-219971/Splash-Screen-has-an-unspecific-Window-Class
	 * for slash windows, jetbrains' products have _NET_WM_WINDOW_TYPE(ATOM) =
	 * _NET_WM_WINDOW_TYPE_NORMAL instead of _NET_WM_WINDOW_TYPE(ATOM) =
	 * _NET_WM_WINDOW_TYPE_SPLASH.
	 * ==> splash windows are tiled instead of being floated.
	 *
	 * check if win name == "splash" then float it regardless of
	 * _NET_WM_WINDOW_TYPE(ATOM) type. this is a hacky solution till jetbrains'
	 * devs fix the splash window type
	 * */
	if (is_splash(win)) {
		map_floating(win);
		raise_window(wm->connection, win);
		xcb_flush(wm->connection);
		return 0;
	}

	if (is_transient(win)) {
		map_floating(win);
		raise_window(wm->connection, win);
		xcb_flush(wm->connection);
		return 0;
	}

	if (!should_manage(win, wm->connection)) {
		return 0;
	}

	const int win_t = window_type(win);

	const int idx	= get_focused_desktop_idx();
	if (idx == -1) {
		log_message(ERROR, "cannot get focused desktop idx");
		return idx;
	}

	switch (win_t) {
	case -1:
	case 0:
	case 1: {
		client_t *client = create_client(win, XCB_ATOM_WINDOW, wm->connection);
		client->state	 = TILED;
		/* log_children(wm->connection, wm->root_window); */
		if (is_tree_empty(wm->desktops[idx]->tree)) {
			return handle_first_window(client, wm->desktops[idx]);
		}
		log_tree_nodes(wm->desktops[idx]->tree);
		return handle_subsequent_window(client, wm->desktops[idx]);
	}
	case 2: {
		if (wbar == XCB_NONE) {
			wbar = win;
		} else {
			return 0;
		}
		rectangle_t				  rc = {0};
		xcb_get_geometry_reply_t *g	 = get_geometry(win, wm->connection);
		if (g == NULL) {
			log_message(ERROR, "cannot get %d geometry", win);
			return -1;
		}
		rc.height = g->height;
		rc.width  = g->width;
		rc.x	  = g->x;
		rc.y	  = g->y;
		free(g);
		g = NULL;
		if (!is_tree_empty(wm->desktops[idx]->tree)) {
			wm->desktops[idx]->tree->rectangle.height =
				(uint16_t)(wm->screen->height_in_pixels - W_GAP - rc.height -
						   5);
			wm->desktops[idx]->tree->rectangle.y = (int16_t)(rc.height + 5);
			resize_subtree(wm->desktops[idx]->tree);
			display_bar(rc, win);
			return render_tree(wm->desktops[idx]->tree);
		}
		display_bar(rc, win);
		return 0;
	}
	case 3:
	case 4:
	case 5:
	case 6: {
		map_floating(win);
		raise_window(wm->connection, win);
		xcb_flush(wm->connection);
	}

	default: break;
	}
	return 0;
}

int handle_enter_notify(xcb_enter_notify_event_t *ev) {
	xcb_window_t win		   = ev->event;
	xcb_window_t active_window = XCB_NONE;

	if (ev->mode != XCB_NOTIFY_MODE_NORMAL ||
		ev->detail == XCB_NOTIFY_DETAIL_INFERIOR) {
		return 0;
	}

	if (win == wbar) {
		log_message(DEBUG, "pointer on wbar");
		return 0;
	}

	if (!window_exists(wm->connection, win)) {
		return 0;
	}

	const int curd = get_focused_desktop_idx();
	if (curd == -1) {
		return curd;
	}

	node_t *root = wm->desktops[curd]->tree;

	node_t *n	 = find_node_by_window_id(root, win);
	if (n == NULL) {
		return -1;
	}

	if (win == wm->root_window) {
		// return 0;
		log_message(DEBUG, "pointer on root");
	}

	xcb_get_property_cookie_t c =
		xcb_ewmh_get_active_window(wm->ewmh, wm->screen_nbr);
	// uint8_t w =
	xcb_ewmh_get_active_window_reply(wm->ewmh, c, &active_window, NULL);
	// what was I thinking here
	if (active_window != n->client->window && n->client->state != FULLSCREEN) {
		const int r = set_active_window_name(win);
		if (r != 0 ||
			change_border_attr(
				wm->connection, n->client->window, 0x83a598, 2, true) != 0) {
			return -1;
		}
		// if (r != 0 ||
		// 	change_border_attr(
		// 		wm->connection, n->client->window, 0x000000, 0, true) != 0) {
		// 	return -1;
		// }
	}

	// n->is_focused = true;
	// raise_window(wm->connection, n->client->window);

	return 0;
}

int handle_leave_notify(xcb_leave_notify_event_t *ev) {
	xcb_window_t win = ev->event;

	if (win == wbar) {
		return 0;
	}

	if (ev->mode != XCB_NOTIFY_MODE_NORMAL ||
		ev->detail == XCB_NOTIFY_DETAIL_INFERIOR) {
		return 0;
	}

	if (!window_exists(wm->connection, win)) {
		return 0;
	}

	const int curd = get_focused_desktop_idx();
	if (curd == -1) {
		return -1;
	}

	node_t		*root		   = wm->desktops[curd]->tree;
	xcb_window_t active_window = XCB_NONE;
	node_t		*n			   = find_node_by_window_id(root, win);

	if (n == NULL) {
		return 0;
	}

	xcb_get_property_cookie_t c =
		xcb_ewmh_get_active_window(wm->ewmh, wm->screen_nbr);
	// uint8_t w =
	xcb_ewmh_get_active_window_reply(wm->ewmh, c, &active_window, NULL);
	if (active_window != n->client->window) {
		return 0;
	}

	if (change_border_attr(
			wm->connection, n->client->window, 0x000000, 0, false) != 0) {
		log_message(ERROR,
					"Failed to change border attr for window %d\n",
					n->client->window);
		return -1;
	}

	lower_window(wm->connection, n->client->window);
	// n->is_focused = false;
	return 0;
}

void handle_key_press(xcb_key_press_event_t *key_press) {
	uint16_t	 cleaned_state = (key_press->state & ~(0 | XCB_MOD_MASK_LOCK));
	xcb_keysym_t k			   = get_keysym(key_press->detail, wm->connection);
	size_t		 n			   = sizeof(keys_) / sizeof(keys_[0]);
	for (size_t i = 0; i < n; ++i) {
		if (cleaned_state == (keys_[i].mod & ~(0 | XCB_MOD_MASK_LOCK))) {
			if (keys_[i].keysym == k) {
				arg_t *a   = keys_[i].arg;
				int	   ret = keys_[i].function_ptr(a);
				if (ret != 0) {
					log_message(ERROR,
								"error while executing function_ptr(..)");
				}
				break;
			}
		}
	}
}

int handle_client_message(xcb_client_message_event_t *client_message) {
	if (client_message->format != 32) {
		return 0;
	}

	int d = get_focused_desktop_idx();
	if (d == -1) {
		return d;
	}

	node_t *root = wm->desktops[d]->tree;

	node_t *n	 = find_node_by_window_id(root, client_message->window);
	if (n == NULL) {
		return 0;
	}
	if (client_message->type == wm->ewmh->_NET_CURRENT_DESKTOP) {
		uint32_t nd = client_message->data.data32[0];
		if (nd > wm->ewmh->_NET_NUMBER_OF_DESKTOPS - 1) {
			return -1;
		}
		if (switch_desktop(nd) != 0) {
			return -1;
		}
		/* log_message(DEBUG, "received data32:\n"); */
		/* for (ulong i = 0; i < LENGTH(client_message->data.data32);
		 * i++) {
		 */
		/*     log_message(DEBUG, "data32[%d]: %u\n", i,
		 * client_message->data.data32[i]); */
		/* } */
		// log_active_desktop(w);
		return 0;
	} else if (client_message->type == wm->ewmh->_NET_CLOSE_WINDOW) {
		log_message(
			INFO, "window want to be closed %d", client_message->window);
	} else if (client_message->type == wm->ewmh->_NET_WM_STATE) {
		if (((unsigned)client_message->data.data32[1] ==
				 wm->ewmh->_NET_WM_STATE_FULLSCREEN ||
			 (unsigned)client_message->data.data32[2] ==
				 wm->ewmh->_NET_WM_STATE_FULLSCREEN)) {
			// uint32_t mode = client_message->data.data32[0];
			// enum {
			// 	_NET_WM_STATE_REMOVE,
			// 	_NET_WM_STATE_ADD,
			// 	_NET_WM_STATE_TOGGLE
			// };
			// if (mode == _NET_WM_STATE_TOGGLE)
			// 	mode = (n->client->state == FULLSCREEN) ?
			// _NET_WM_STATE_REMOVE
			// : _NET_WM_STATE_ADD; set_fullscreen(n, wm, mode ==
			// _NET_WM_STATE_ADD);
			log_message(INFO,
						"window wants to be fullscreend %d",
						client_message->window);
		}
	}
	// TODO: ewmh->_NET_WM_STATE
	// TODO: ewmh->_NET_CLOSE_WINDOW
	// TODO: ewmh->_NET_WM_DESKTOP
	// TODO: ewmh->_NET_WM_STATE_FULLSCREEN
	// TODO: ewmh->_NET_ACTIVE_WINDOW

	return 0;
}

int handle_unmap_notify(xcb_window_t win) {
	int idx = get_focused_desktop_idx();
	if (idx == -1) {
		return -1;
	}

	node_t *root = wm->desktops[idx]->tree;
	if (root == NULL) {
		return -1;
	}

	if (!client_exist(root, win)) {
		log_message(INFO, "cannot find win %d", win);
		log_tree_nodes(root);
		return 0;
	}

	if (kill_window(win) != 0) {
		log_message(INFO, "error kill unmap");
		return -1;
	}

	return 0;
}

void handle_configure_request(xcb_configure_request_event_t *e) {
	xcb_icccm_get_text_property_reply_t t_reply;
	xcb_get_property_cookie_t			cn =
		xcb_icccm_get_wm_name(wm->connection, e->window);
	const uint8_t wr =
		xcb_icccm_get_wm_name_reply(wm->connection, cn, &t_reply, NULL);
	char name[256];
	if (wr == 1) {
		snprintf(name, sizeof(name), "%s", t_reply.name);
		xcb_icccm_get_text_property_reply_wipe(&t_reply);
	} else {
		return;
	}

	log_message(INFO,
				"window %d  name %s wants to be at %dx%d with %dx%d\n",
				e->window,
				name,
				e->x,
				e->y,
				e->width,
				e->height);

	const int d = get_focused_desktop_idx();
	if (d == -1) {
		return;
	}

	node_t *n		   = wm->desktops[d]->tree;
	bool	is_managed = client_exist(n, e->window);
	if (!is_managed) {
		uint16_t mask = 0;
		uint32_t values[7];
		uint16_t i = 0;
		if (e->value_mask & XCB_CONFIG_WINDOW_X) {
			mask |= XCB_CONFIG_WINDOW_X;
			values[i++] = (uint32_t)e->x;
		}

		if (e->value_mask & XCB_CONFIG_WINDOW_Y) {
			mask |= XCB_CONFIG_WINDOW_Y;
			values[i++] = (uint32_t)e->y;
		}

		if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
			mask |= XCB_CONFIG_WINDOW_WIDTH;
			values[i++] = e->width;
		}

		if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
			mask |= XCB_CONFIG_WINDOW_HEIGHT;
			values[i++] = e->height;
		}

		if (e->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) {
			mask |= XCB_CONFIG_WINDOW_BORDER_WIDTH;
			values[i++] = e->border_width;
		}

		if (e->value_mask & XCB_CONFIG_WINDOW_SIBLING) {
			mask |= XCB_CONFIG_WINDOW_SIBLING;
			values[i++] = e->sibling;
		}

		if (e->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
			mask |= XCB_CONFIG_WINDOW_STACK_MODE;
			values[i++] = e->stack_mode;
		}

		xcb_configure_window(wm->connection, e->window, mask, values);
		xcb_flush(wm->connection);
	} else {
		const node_t *node = find_node_by_window_id(n, e->window);
		if (node == NULL) {
			log_message(DEBUG,
						"config request -> cannot find node with win id %d",
						e->window);
			return;
		}
		// TODO: deal with *node
	}
}

int handle_destroy_notify(xcb_window_t win) {
	int idx = get_focused_desktop_idx();
	if (idx == -1) {
		return -1;
	}

	node_t *root = wm->desktops[idx]->tree;
	if (root == NULL) {
		return -1;
	}

	if (!client_exist(root, win)) {
		log_message(INFO, "cannot find win %d", win);
		log_tree_nodes(root);
		return 0;
	}

	if (kill_window(win) != 0) {
		log_message(INFO, "error kill destory");
		return -1;
	}

	return 0;
}

void log_active_desktop() {
	for (int i = 0; i < wm->n_of_desktops; i++) {
		log_message(DEBUG,
					"desktop %d, active %s",
					wm->desktops[i]->id,
					wm->desktops[i]->is_focused ? "true" : "false");
	}
}

void log_children(xcb_conn_t *connection, xcb_window_t root_window) {
	xcb_query_tree_cookie_t tree_cookie =
		xcb_query_tree(connection, root_window);
	xcb_query_tree_reply_t *tree_reply =
		xcb_query_tree_reply(connection, tree_cookie, NULL);
	if (tree_reply == NULL) {
		log_message(ERROR, "Failed to query tree reply\n");
		return;
	}

	log_message(DEBUG, "Children of root window:\n");
	xcb_window_t *children	   = xcb_query_tree_children(tree_reply);
	const int	  num_children = xcb_query_tree_children_length(tree_reply);
	for (int i = 0; i < num_children; ++i) {
		xcb_icccm_get_text_property_reply_t t_reply;
		xcb_get_property_cookie_t			cn =
			xcb_icccm_get_wm_name(connection, children[i]);
		uint8_t wr =
			xcb_icccm_get_wm_name_reply(connection, cn, &t_reply, NULL);
		if (wr == 1) {
			log_message(DEBUG, "Child %d: %s\n", i + 1, t_reply.name);
			xcb_icccm_get_text_property_reply_wipe(&t_reply);
		} else {
			log_message(
				DEBUG, "Failed to get window name for child %d\n", i + 1);
		}
	}

	free(tree_reply);
}

void free_desktops(desktop_t **d, int n) {
	for (int i = 0; i < n; ++i) {
		free_tree(d[i]->tree);
		free(d[i]);
		d[i] = NULL;
	}
	free(d);
	d = NULL;
}

int main() {

	wm = init_wm();
	if (wm == 0x00) {
		log_message(ERROR, "Failed to initialize window manager\n");
		exit(EXIT_FAILURE);
	}

	// if (parse_config("$HOME/_dev/c_dev//zwm.conf", &conf) != 0) {
	// 	return -1;
	// }

	polybar_exec("~/_dev/c_dev/zwm/config.ini");
	xcb_flush(wm->connection);

	xcb_generic_event_t *event;

	while ((event = xcb_wait_for_event(wm->connection))) {
		switch (event->response_type & ~0x80) {
		case XCB_MAP_REQUEST: {
			xcb_map_request_event_t *map_request =
				(xcb_map_request_event_t *)event;
			if (handle_map_request(map_request->window) != 0) {
				log_message(ERROR,
							"Failed to handle MAP_REQUEST for window %d\n",
							map_request->window);
			}
			break;
		}
		case XCB_UNMAP_NOTIFY: {
			xcb_unmap_notify_event_t *unmap_notify =
				(xcb_unmap_notify_event_t *)event;
			if (handle_unmap_notify(unmap_notify->window) != 0) {
				log_message(ERROR,
							"Failed to handle XCB_UNMAP_NOTIFY for window %d\n",
							unmap_notify->window);
				/* exit(EXIT_FAILURE); */
			}
			break;
		}
		case XCB_DESTROY_NOTIFY: {
			xcb_destroy_notify_event_t *destroy_notify =
				(xcb_destroy_notify_event_t *)event;
			if (handle_destroy_notify(destroy_notify->window) != 0) {
				log_message(ERROR,
							"Failed to handle XCB_DESTROY_NOTIFY for "
							"window %d\n",
							destroy_notify->window);
			}
			break;
		}
		case XCB_EXPOSE: {
			__attribute__((unused)) xcb_expose_event_t *expose_event =
				(xcb_expose_event_t *)event;
			break;
		}
		case XCB_CLIENT_MESSAGE: {
			xcb_client_message_event_t *client_message =
				(xcb_client_message_event_t *)event;
			handle_client_message(client_message);
			break;
		}
		case XCB_CONFIGURE_REQUEST: {
			xcb_configure_request_event_t *config_request =
				(xcb_configure_request_event_t *)event;
			handle_configure_request(config_request);
			break;
		}
		case XCB_CONFIGURE_NOTIFY: {
			__attribute__((unused))
			xcb_configure_notify_event_t *config_notify =
				(xcb_configure_notify_event_t *)event;
			/* log_message(DEBUG, "XCB_CONFIGURE_NOTIFY %d\n",
			 * config_notify->window); */
			break;
		}
		case XCB_PROPERTY_NOTIFY: {
			__attribute__((unused))
			xcb_property_notify_event_t *property_notify =
				(xcb_property_notify_event_t *)event;
			/* log_message(DEBUG, "XCB_PROPERTY_NOTIFY %d\n",
			 * property_notify->window); */
			break;
		}
		case XCB_ENTER_NOTIFY: {
			xcb_enter_notify_event_t *enter_event =
				(xcb_enter_notify_event_t *)event;
			if (handle_enter_notify(enter_event) != 0) {
				log_message(ERROR,
							"Failed to handle XCB_ENTER_NOTIFY for window %d\n",
							enter_event->event);
				/* exit(EXIT_FAILURE); */
			}
			break;
		}
		case XCB_LEAVE_NOTIFY: {
			xcb_leave_notify_event_t *leave_event =
				(xcb_leave_notify_event_t *)event;
			if (handle_leave_notify(leave_event) != 0) {
				log_message(ERROR,
							"Failed to handle XCB_LEAVE_NOTIFY for window %d\n",
							leave_event->event);
				/* exit(EXIT_FAILURE); */
			}
			break;
		}
		case XCB_MOTION_NOTIFY: {
			__attribute__((unused)) xcb_motion_notify_event_t *motion_notify =
				(xcb_motion_notify_event_t *)event;
			break;
		}
		case XCB_BUTTON_PRESS: {
			__attribute__((unused)) xcb_button_press_event_t *button_press =
				(xcb_button_press_event_t *)event;
			break;
		}
		case XCB_BUTTON_RELEASE: {
			__attribute__((unused)) xcb_button_release_event_t *button_release =
				(xcb_button_release_event_t *)event;
			break;
		}
		case XCB_KEY_PRESS: {
			xcb_key_press_event_t *key_press = (xcb_key_press_event_t *)event;
			// log_message(DEBUG, "XCB_KEY_PRESS");
			handle_key_press(key_press);
			break;
		}
		case XCB_KEY_RELEASE: {
			__attribute__((unused)) xcb_key_release_event_t *key_release =
				(xcb_key_release_event_t *)event;
			break;
		}
		case XCB_FOCUS_IN: {
			__attribute__((unused)) xcb_focus_in_event_t *focus_in_event =
				(xcb_focus_in_event_t *)event;
			// log_message(DEBUG, "XCB_FOCUS_IN");
			break;
		}
		case XCB_FOCUS_OUT: {
			__attribute__((unused)) xcb_focus_out_event_t *focus_out_event =
				(xcb_focus_out_event_t *)event;
			break;
		}
		case XCB_MAPPING_NOTIFY: {
			__attribute__((unused)) xcb_mapping_notify_event_t *mapping_notify =
				(xcb_mapping_notify_event_t *)event;
			if (0 != grab_keys(wm->connection, wm->root_window)) {
				log_message(ERROR, "cannot grab keys");
			}
			break;
		}
		default: {
			/* log_message(DEBUG, "event %d", event->response_type &
			 * ~0x80);
			 */
			break;
		}
		}
		free(event);
	}
	xcb_disconnect(wm->connection);
	free_desktops(wm->desktops, wm->n_of_desktops);
	xcb_ewmh_connection_wipe(wm->ewmh);
	free(wm);
	wm = NULL;
	return 0;
}
