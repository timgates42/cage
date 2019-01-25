/*
 * Cage: A Wayland kiosk.
 * 
 * Copyright (C) 2018-2019 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200112L

#include "config.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server.h>
#include <wlr/backend.h>
#include <wlr/backend/wayland.h>
#include <wlr/backend/x11.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_idle.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_output_layout.h>
#if CAGE_HAS_XWAYLAND
#include <wlr/types/wlr_xcursor_manager.h>
#endif
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#if CAGE_HAS_XWAYLAND
#include <wlr/xwayland.h>
#endif

#include "idle_inhibit_v1.h"
#include "output.h"
#include "seat.h"
#include "server.h"
#include "view.h"
#include "xdg_shell.h"
#if CAGE_HAS_XWAYLAND
#include "xwayland.h"
#endif

void
set_window_title(struct cg_server *server, struct cg_view *view)
{
	struct wlr_output *output = server->output->wlr_output;
	bool is_wl = wlr_output_is_wl(output);
	bool is_x11 = wlr_output_is_x11(output);

	if (!is_wl && !is_x11) {
		return;
	}

	char *title = view_get_title(view);
	if (is_wl) {
		wlr_wl_output_set_title(output, title);
	} else if (is_x11) {
		wlr_x11_output_set_title(output, title);
	}
	free(title);
}

static bool
spawn_primary_client(char *argv[], pid_t *pid_out)
{
	pid_t pid = fork();
	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(1);
	} else if (pid == -1) {
		wlr_log_errno(WLR_ERROR, "Unable to fork");
		return false;
	}

	*pid_out = pid;
	wlr_log(WLR_DEBUG, "Child process created with pid %d", pid);
	return true;
}

static int
handle_signal(int signal, void *data)
{
	struct wl_display *display = data;

	switch (signal) {
	case SIGINT:
		/* Fallthrough */
	case SIGTERM:
		wl_display_terminate(display);
		return 0;
	default:
		return 1;
	}
}

int
main(int argc, char *argv[])
{
	struct cg_server server = {0};
	struct wl_event_loop *event_loop = NULL;
	struct wlr_renderer *renderer = NULL;
	struct wlr_compositor *compositor = NULL;
	struct wlr_data_device_manager *data_device_mgr = NULL;
	struct wlr_xdg_shell *xdg_shell = NULL;
#if CAGE_HAS_XWAYLAND
	struct wlr_xwayland *xwayland = NULL;
	struct wlr_xcursor_manager *xcursor_manager = NULL;
#endif
	int ret = 0;

	if (argc < 2) {
		printf("Usage: %s APPLICATION\n", argv[0]);
		return 1;
	}

#ifdef DEBUG
	wlr_log_init(WLR_DEBUG, NULL);
#else
	wlr_log_init(WLR_ERROR, NULL);
#endif

	server.wl_display = wl_display_create();
	if (!server.wl_display) {
		wlr_log(WLR_ERROR, "Cannot allocate a Wayland display");
		return 1;
	}

	event_loop = wl_display_get_event_loop(server.wl_display);
	wl_event_loop_add_signal(event_loop, SIGINT, handle_signal, &server.wl_display);
	wl_event_loop_add_signal(event_loop, SIGTERM, handle_signal, &server.wl_display);

	server.backend = wlr_backend_autocreate(server.wl_display, NULL);
	if (!server.backend) {
		wlr_log(WLR_ERROR, "Unable to create the wlroots backend");
		ret = 1;
		goto end;
	}

	renderer = wlr_backend_get_renderer(server.backend);
	wlr_renderer_init_wl_display(renderer, server.wl_display);

	server.output_layout = wlr_output_layout_create();
	if (!server.output_layout) {
		wlr_log(WLR_ERROR, "Unable to create output layout");
		ret = 1;
		goto end;
	}

	compositor = wlr_compositor_create(server.wl_display, renderer);
	if (!compositor) {
		wlr_log(WLR_ERROR, "Unable to create the wlroots compositor");
		ret = 1;
		goto end;
	}

	data_device_mgr = wlr_data_device_manager_create(server.wl_display);
	if (!data_device_mgr) {
		wlr_log(WLR_ERROR, "Unable to create the data device manager");
		ret = 1;
		goto end;
	}

	/* Configure a listener to be notified when new outputs are
	 * available on the backend. We use this only to detect the
	 * first output and ignore subsequent outputs. */
	server.new_output.notify = handle_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	server.seat = cg_seat_create(&server);
	if (!server.seat) {
		wlr_log(WLR_ERROR, "Unable to create the seat");
		ret = 1;
		goto end;
	}

	server.idle = wlr_idle_create(server.wl_display);
	if (!server.idle) {
		wlr_log(WLR_ERROR, "Unable to create the idle tracker");
		ret = 1;
		goto end;
	}

	server.idle_inhibit_v1 = wlr_idle_inhibit_v1_create(server.wl_display);
	if (!server.idle_inhibit_v1) {
		wlr_log(WLR_ERROR, "Cannot create the idle inhibitor");
		ret = 1;
		goto end;
	}
	server.new_idle_inhibitor_v1.notify = handle_idle_inhibitor_v1_new;
	wl_signal_add(&server.idle_inhibit_v1->events.new_inhibitor, &server.new_idle_inhibitor_v1);
	wl_list_init(&server.inhibitors);
		
	xdg_shell = wlr_xdg_shell_create(server.wl_display);
	if (!xdg_shell) {
		wlr_log(WLR_ERROR, "Unable to create the XDG shell interface");
		ret = 1;
		goto end;
	}
	wl_list_init(&server.views);
	server.new_xdg_shell_surface.notify = handle_xdg_shell_surface_new;
	wl_signal_add(&xdg_shell->events.new_surface, &server.new_xdg_shell_surface);

#if CAGE_HAS_XWAYLAND
	xwayland = wlr_xwayland_create(server.wl_display, compositor, true);
	if (!xwayland) {
		wlr_log(WLR_ERROR, "Cannot create XWayland server");
		ret = 1;
		goto end;
	}
	server.new_xwayland_surface.notify = handle_xwayland_surface_new;
	wl_signal_add(&xwayland->events.new_surface, &server.new_xwayland_surface);

	xcursor_manager = wlr_xcursor_manager_create(DEFAULT_XCURSOR, XCURSOR_SIZE);
	if (!xcursor_manager) {
		wlr_log(WLR_ERROR, "Cannot create XWayland XCursor manager");
	        ret = 1;
		goto end;
	}
	if (wlr_xcursor_manager_load(xcursor_manager, 1)) {
		wlr_log(WLR_ERROR, "Cannot load XWayland XCursor theme");
	}
	struct wlr_xcursor *xcursor =
		wlr_xcursor_manager_get_xcursor(xcursor_manager, DEFAULT_XCURSOR, 1);
	if (xcursor) {
		struct wlr_xcursor_image *image = xcursor->images[0];
		wlr_xwayland_set_cursor(xwayland, image->buffer,
					image->width * 4, image->width, image->height,
					image->hotspot_x, image->hotspot_y);
	}
#endif

	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_log_errno(WLR_ERROR, "Unable to open Wayland socket");
		ret = 1;
	        goto end;
	}

	if (!wlr_backend_start(server.backend)) {
		wlr_log(WLR_ERROR, "Unable to start the wlroots backend");
		ret = 1;
		goto end;
	}

	int rc = setenv("WAYLAND_DISPLAY", socket, true);
	if (rc < 0) {
		wlr_log_errno(WLR_ERROR, "Unable to set WAYLAND_DISPLAY.",
			      "Clients may not be able to connect");
	} else {
		wlr_log(WLR_DEBUG, "Cage is running on Wayland display %s", socket);
	}

#if CAGE_HAS_XWAYLAND
	wlr_xwayland_set_seat(xwayland, server.seat->seat);
#endif

	pid_t pid;
	if (!spawn_primary_client(argv + 1, &pid)) {
		ret = 1;
		goto end;
	}

	wl_display_run(server.wl_display);

#if CAGE_HAS_XWAYLAND
	wlr_xwayland_destroy(xwayland);
	wlr_xcursor_manager_destroy(xcursor_manager);
#endif
	wl_display_destroy_clients(server.wl_display);

	waitpid(pid, NULL, 0);

end:
	cg_seat_destroy(server.seat);
	wlr_xdg_shell_destroy(xdg_shell);
	wlr_idle_inhibit_v1_destroy(server.idle_inhibit_v1);
	if (server.idle) {
		wlr_idle_destroy(server.idle);
	}
	wlr_data_device_manager_destroy(data_device_mgr);
	wlr_compositor_destroy(compositor);
	wlr_output_layout_destroy(server.output_layout);
	wlr_backend_destroy(server.backend);
	/* This function is not null-safe, but we only ever get here
	   with a proper wl_display. */
	wl_display_destroy(server.wl_display);
	return ret;
}
