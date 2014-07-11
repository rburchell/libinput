/*
 * Copyright © 2014 Red Hat, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#define _GNU_SOURCE
#include <config.h>

#include <linux/input.h>

#include <assert.h>
#include <cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/utsname.h>

#include <gtk/gtk.h>
#include <glib.h>

#include <libevdev/libevdev.h>
#include <libinput.h>
#include <libinput-util.h>

#define clip(val_, min_, max_) min((max_), max((min_), (val_)))

#define NUM_TRAINING_TARGETS 5
#define NUM_STUDY_TARGETS 15
#define NUM_SETS 3

enum study_state {
	STATE_WELCOME,
	STATE_CONFIRM_DEVICE,
	STATE_TRAINING,
	STATE_INTERMISSION,
	STATE_STUDY_START,
	STATE_STUDY,
	STATE_DONE,
};

struct study {
	enum study_state state;
	enum study_state new_state; /* changed on release */

	int object_x,
	    object_y;
	int object_radius;
	int last_random;

	int ntargets;

	int fd;
	char *filename;
	char *cwd;

	int set;
	int radii[NUM_SETS];

	/* the device used during the study */
	struct libinput_device *device;
};

struct touch {
	int active;
	int x, y;
};

struct device {
	struct list node;
	struct libinput_device *dev;
};

struct window {
	struct study base;

	GtkWidget *win;
	GtkWidget *area;
	int width, height; /* of window */

	/* sprite position */
	double x, y;

	/* abs position */
	int absx, absy;

	/* scroll bar positions */
	int vx, vy;
	int hx, hy;

	/* touch positions */
	struct touch touches[32];

	/* l/m/r mouse buttons */
	int l, m, r;

	struct list device_list;
};

static int
error(const char *fmt, ...)
{
	va_list args;
	fprintf(stderr, "error: ");

	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);

	return EXIT_FAILURE;
}

static void
msg(const char *fmt, ...)
{
	va_list args;
	printf("info: ");

	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
}

static void
usage_device(void)
{
	printf("To function correctly, this tool needs read access to \n"
	       "the device used during analysis. Run it as root, or enable\n"
	       "read access on the /dev/input/event<N> devices that you\n"
	       "want to use during the study. e.g. \n"
	       "	sudo chmod o+r /dev/input/event7\n");
}

static void
usage(void)
{
	printf("%s [path/to/device]\n", program_invocation_short_name);
	printf("\n"
	       "This  tool runs a basic user-study, analyzing input events \n"
	       "from pointer devices.\n"
	       "\n");
	usage_device();

}

static void
study_default_target(struct window *w)
{
	w->base.object_x = w->width/2;
	w->base.object_y = w->height * 0.75;
	w->base.object_radius = 50;
}

static void
study_show_text(cairo_t *cr, struct window *w)
{
	struct study *s = &w->base;
	const int font_size = 14;
	const char **str;
	int line;

	const char *training_message[] = {
		"Click on the targets as they appear.",
		NULL
	};

	const char *start_message[] = {
		"Click on the target to start the study.",
		NULL
	};

	switch(s->state) {
	case STATE_TRAINING:
	case STATE_STUDY:
		str = training_message;
		break;
	case STATE_STUDY_START:
	case STATE_INTERMISSION:
		str = start_message;
		break;
	default:
		return;
	}

	cairo_save(cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_set_font_size(cr, font_size);

	line = 0;

	while (*str != 0) {
		cairo_move_to(cr, 400, 100 + line * font_size * 1.2);
		cairo_show_text(cr, *str);
		str++;
		line++;
	}

	cairo_restore(cr);

}

static void
study_init(struct window *w)
{
	struct study *s = &w->base;
	int radii[] = { 15, 30, 45 };
	int i;

	study_default_target(w);
	s->state = STATE_WELCOME;
	s->new_state = STATE_WELCOME;
	s->filename = NULL;
	s->cwd = NULL;

	s->ntargets = NUM_STUDY_TARGETS;

	/* Define order at startup, but randomly */
	for (i = 0; i < NUM_SETS; i++)
		s->radii[i] = radii[i];
	for (i = NUM_SETS - 1; i > 0; i--) {
		int j = rand() % (i + 1);
		int tmp = s->radii[j];
		s->radii[j] = s->radii[i];
		s->radii[i] = tmp;
	}

}

static void
study_cleanup(struct window *w)
{
	struct study *s = &w->base;

	free(s->filename);
	free(s->cwd);
}

static void
study_draw_object(cairo_t *cr, struct window *w)
{
	struct study *s = &w->base;

	/* draw the click object */
	cairo_save(cr);
	if (s->state == STATE_TRAINING ||
	    s->state  == STATE_STUDY_START ||
	    s->state == STATE_INTERMISSION ||
	    s->state  == STATE_STUDY)
		cairo_set_source_rgb(cr, .4, .8, 0);
	else
		cairo_set_source_rgb(cr, .0, .2, .8);
	cairo_arc(cr, s->object_x, s->object_y, s->object_radius, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_restore(cr);
}

static gboolean
draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
	struct window *w = data;
	struct study *s = &w->base;
#if 0
	struct touch *t;
#endif

	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_rectangle(cr, 0, 0, w->width, w->height);
	cairo_fill(cr);

	if (s->state != STATE_CONFIRM_DEVICE &&
	    s->state != STATE_TRAINING &&
	    s->state != STATE_STUDY_START &&
	    s->state != STATE_INTERMISSION &&
	    s->state != STATE_STUDY)
		return TRUE;

	/* Study elements */
	study_show_text(cr, w);
	study_draw_object(cr, w);

	/* draw pointer sprite */
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_save(cr);
	cairo_move_to(cr, w->x, w->y);
	cairo_rel_line_to(cr, 10, 15);
	cairo_rel_line_to(cr, -10, 0);
	cairo_rel_line_to(cr, 0, -15);
	cairo_fill(cr);
	cairo_restore(cr);


#if 0
	/* draw scroll bars */
	cairo_set_source_rgb(cr, .4, .8, 0);

	cairo_save(cr);
	cairo_rectangle(cr, w->vx - 10, w->vy - 20, 20, 40);
	cairo_rectangle(cr, w->hx - 20, w->hy - 10, 40, 20);
	cairo_fill(cr);
	cairo_restore(cr);

	/* touch points */
	cairo_set_source_rgb(cr, .8, .2, .2);

	ARRAY_FOR_EACH(w->touches, t) {
		cairo_save(cr);
		cairo_arc(cr, t->x, t->y, 10, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_restore(cr);
	}

	/* abs position */
	cairo_set_source_rgb(cr, .2, .4, .8);

	cairo_save(cr);
	cairo_arc(cr, w->absx, w->absy, 10, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_restore(cr);

	/* lmr buttons */
	cairo_save(cr);
	if (w->l || w->m || w->r) {
		cairo_set_source_rgb(cr, .2, .8, .8);
		if (w->l)
			cairo_rectangle(cr, w->width/2 - 100, w->height - 200, 70, 30);
		if (w->m)
			cairo_rectangle(cr, w->width/2 - 20, w->height - 200, 40, 30);
		if (w->r)
			cairo_rectangle(cr, w->width/2 + 30, w->height - 200, 70, 30);
		cairo_fill(cr);
	}

	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_rectangle(cr, w->width/2 - 100, w->height - 200, 70, 30);
	cairo_rectangle(cr, w->width/2 - 20, w->height - 200, 40, 30);
	cairo_rectangle(cr, w->width/2 + 30, w->height - 200, 70, 30);
	cairo_stroke(cr);
	cairo_restore(cr);
#endif

	return TRUE;
}

static void
study_screen_too_small_error(struct window *w)
{
	const char *message;
	GtkWidget *dialog;

	message = "Sorry, your screen does not meet the"
		  " minimum requirements for this study.";
	gdk_window_set_cursor(gtk_widget_get_window(w->win),
			      NULL);

	dialog = gtk_message_dialog_new(GTK_WINDOW(w->win),
					GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
					GTK_MESSAGE_ERROR,
					GTK_BUTTONS_CLOSE,
					message);
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
}

static int
study_show_welcome_message(struct window *w)
{
	const char *message;
	GtkWidget *dialog;
	gint response;

	message = "Thank you for participating in this study. The goal of this study\n"
		 "is to analyze the pointer acceleration code. The study\n"
		 "consists of several randomized sets of moving targets.\n"
		 "Your task is to simply click on these targets as they appear\n"
		 "using a mouse-like input device.\n"
		 "\n"
		 "The data collected by this program is limited to:\n"
		 "- your kernel version (see uname(2))\n"
		 "- DMI device information (see /sys/class/dmi/id)\n"
		 "- input device name and capabilities (see evtest(1))\n"
		 "- input events with timestamps\n"
		 "- converted events and timestamps\n"
		 "\n"
		 "No data that can personally identify you is collected.\n"
		 "Key events are received by this program but not collected or\n"
		 "analyzed.\n"
		 "\n"
		 "The data collected is available in a plain text file and must\n"
		 "be sent to me via email. This tool does not send any data.\n"
		 "\n"
		 "You can abort any time by hitting Esc.\n"
		 "\n"
		 "When you're ready to go please click OK\n"
		 "Press Cancel to abort and exit this study\n";
	gdk_window_set_cursor(gtk_widget_get_window(w->win),
			      NULL);

	dialog = gtk_message_dialog_new(GTK_WINDOW(w->win),
					GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
					GTK_MESSAGE_INFO,
					GTK_BUTTONS_OK_CANCEL,
					message);
	response = gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);

	if (response == GTK_RESPONSE_CANCEL) {
		gtk_main_quit();
		return -1;
	}

	return 0;
}

static int
study_show_confirm_message(struct window *w)
{
	const char *message;
	GtkWidget *dialog;
	gint response;

	message = "Almost ready to go. This is an unsupervised study and\n"
		  "we ask you to confirm the following:\n"
		  "\n"
		  "1) You have normal or corrected vision and you can identify\n"
		  "   objects on the screen easily\n"
		  "2) You acknowledge that this tool will collect real-time events\n"
		  "   from the device used during the study, and only that device\n"
		  "3) You are familiar with interacting a computer desktop environment\n"
		  "   and you have no motoric challenges that impede on using a mouse\n"
		  "4) You accept that the collected data may be used to alter and improve\n"
		  "   interactions with the desktop environment.\n"
		  "5) You accept that the data may be used in a publicly available\n"
		  "6) You accept that the raw data may be made available to other\n"
		  "   researchers for further analysis. All effort is made to avoid\n"
		  "   any attempt at personal identification of the data.\n"
		  "7) You agree not to modify the collected data of this study\n"
		  "   before submission\n"
		  "\n"
		  "If you agree with the above, please hit Yes\n"
		  "If you disagree with the above, please hit No to quit\n"
		  "\n"
		 "You can abort any time by hitting Esc.\n";

	gdk_window_set_cursor(gtk_widget_get_window(w->win),
			      NULL);

	dialog = gtk_message_dialog_new(GTK_WINDOW(w->win),
					GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
					GTK_MESSAGE_INFO,
					GTK_BUTTONS_YES_NO,
					message);
	response = gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);

	if (response == GTK_RESPONSE_NO) {
		gtk_main_quit();
		return -1;
	}

	return 0;
}

static void
study_show_confirm_device(struct window *w)
{
	const char *message;
	GtkWidget *dialog;

	message = "On the next screen, you will see a circle on white background.\n"
		  "Please click on the circle with the device you want to use during\n"
		  "the study. Only data from that device will be collected.\n"
		  "\n"
		  "The device should be a mouse-like device or a touchpad\n"
		  "\n"
		  "Note that the cursor used to select the target is not\n"
		  "your normal system cursor\n"
		  "\n"
		  "You can abort any time by hitting Esc.\n";

	gdk_window_set_cursor(gtk_widget_get_window(w->win),
			      NULL);

	dialog = gtk_message_dialog_new(GTK_WINDOW(w->win),
					GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
					GTK_MESSAGE_INFO,
					GTK_BUTTONS_OK,
					message);
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);

	gdk_window_set_cursor(gtk_widget_get_window(w->win),
			      gdk_cursor_new(GDK_BLANK_CURSOR));
	return;
}

static void
study_show_training_start(struct window *w)
{
	const char *message;
	GtkWidget *dialog;

	message = "Thank you. Your device was selected and events from other\n"
		  "devices will be discarded.\n"
		  "\n"
		  "You are now ready to start a short training session.\n"
		  "With your device, simply click on each target as it appears\n"
		  "\n"
		  "Note that the cursor used to select the targets is not\n"
		  "your normal system cursor\n"
		  "\n"
		  "No events will be collected yet\n"
		  "\n"
		  "You can abort any time by hitting Esc.\n";

	gdk_window_set_cursor(gtk_widget_get_window(w->win),
			      NULL);

	dialog = gtk_message_dialog_new(GTK_WINDOW(w->win),
					GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
					GTK_MESSAGE_INFO,
					GTK_BUTTONS_OK,
					message);
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);

	gdk_window_set_cursor(gtk_widget_get_window(w->win),
			      gdk_cursor_new(GDK_BLANK_CURSOR));

	return;
}

static void
study_show_training_done(struct window *w)
{
	const char *message;
	GtkWidget *dialog;

	message = "Thank you, your training is now complete and we can start\n"
		  "with the actual study.\n"
		  "\n"
		  "The study consists of %d sets of targets of varying size.\n"
		  "There will be a message after each set was completed.\n"
		  "With your device, simply click on each target as it appears\n"
		  "\n"
		  "Note that the cursor used to select the targets is not\n"
		  "your normal system cursor\n"
		  "\n"
		  "Event collection starts once you click the first target\n"
		  "\n"
		  "You can abort any time by hitting Esc.\n";

	gdk_window_set_cursor(gtk_widget_get_window(w->win),
			      NULL);

	dialog = gtk_message_dialog_new(GTK_WINDOW(w->win),
					GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
					GTK_MESSAGE_INFO,
					GTK_BUTTONS_OK,
					message,
					NUM_SETS);
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);

	gdk_window_set_cursor(gtk_widget_get_window(w->win),
			      gdk_cursor_new(GDK_BLANK_CURSOR));

	return;
}

static void
study_show_intermission(struct window *w)
{
	const char *message;
	GtkWidget *dialog;

	message = "This set is now complete. You may have a short rest before\n"
		"the next set starts. Target sizes may change between sets.\n"
		"\n"
		"To start the next set, click OK. Event collection starts\n"
		"when you click the first target\n"
		"\n"
		"You can abort any time by hitting Esc.\n";


	gdk_window_set_cursor(gtk_widget_get_window(w->win),
			      NULL);

	dialog = gtk_message_dialog_new(GTK_WINDOW(w->win),
					GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
					GTK_MESSAGE_INFO,
					GTK_BUTTONS_OK,
					message);
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);

	gdk_window_set_cursor(gtk_widget_get_window(w->win),
			      gdk_cursor_new(GDK_BLANK_CURSOR));

	return;
};


static void
study_show_done(struct window *w)
{
	const char *message;
	GtkWidget *dialog;

	message = "Thank you for completing the study.\n"
		  "\n"
		  "Your results are available in the file\n"
		  "    %s/%s\n"
		  "Please send them unmodified to peter.hutterer@who-t.net, with a subject\n"
		  "of \"userstudy results\"\n";

	gdk_window_set_cursor(gtk_widget_get_window(w->win),
			      NULL);

	dialog = gtk_message_dialog_new(GTK_WINDOW(w->win),
					GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
					GTK_MESSAGE_INFO,
					GTK_BUTTONS_OK,
					message,
					w->base.cwd,
					w->base.filename);
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);

	gdk_window_set_cursor(gtk_widget_get_window(w->win),
			      gdk_cursor_new(GDK_BLANK_CURSOR));
}

static void
study_map_event_cb(struct window *w)
{
	struct study *s = &w->base;

	if (w->width < 1024 || w->height < 768) {
		study_screen_too_small_error(w);
		gtk_main_quit();
		return;
	}

	if (study_show_welcome_message(w) != 0)
		return;

	if (study_show_confirm_message(w) != 0)
		return;

	study_show_confirm_device(w);

	study_default_target(w);
	s->state = STATE_CONFIRM_DEVICE;
}

static void
map_event_cb(GtkWidget *widget, GdkEvent *event, gpointer data)
{
	struct window *w = data;

	gtk_window_get_size(GTK_WINDOW(widget), &w->width, &w->height);

	w->x = w->width/2;
	w->y = w->height/2;

	w->vx = w->width/2;
	w->vy = w->height/2;
	w->hx = w->width/2;
	w->hy = w->height/2;

	g_signal_connect(G_OBJECT(w->area), "draw", G_CALLBACK(draw), w);

	gdk_window_set_cursor(gtk_widget_get_window(w->win),
			      gdk_cursor_new(GDK_BLANK_CURSOR));


	study_map_event_cb(w);
}

static void
window_init(struct window *w)
{
	memset(w, 0, sizeof(*w));

	w->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_widget_set_events(w->win, 0);
	gtk_window_set_title(GTK_WINDOW(w->win), "libinput debugging tool");
	gtk_window_set_default_size(GTK_WINDOW(w->win), 1024, 768);
	gtk_window_maximize(GTK_WINDOW(w->win));
	gtk_window_fullscreen(GTK_WINDOW(w->win));
	gtk_window_set_resizable(GTK_WINDOW(w->win), TRUE);
	gtk_widget_realize(w->win);
	g_signal_connect(G_OBJECT(w->win), "map-event", G_CALLBACK(map_event_cb), w);
	g_signal_connect(G_OBJECT(w->win), "delete-event", G_CALLBACK(gtk_main_quit), NULL);

	w->area = gtk_drawing_area_new();
	gtk_widget_set_events(w->area, 0);
	gtk_container_add(GTK_CONTAINER(w->win), w->area);
	gtk_widget_show_all(w->win);

	list_init(&w->device_list);
}

static void
device_remove(struct device *d)
{
	list_remove(&d->node);
	libinput_device_unref(d->dev);
	free(d);
}

static void
window_cleanup(struct window *w)
{
	struct device *d, *tmp;

	list_for_each_safe(d, tmp, &w->device_list, node)
		device_remove(d);
}

static void
change_ptraccel(struct window *w, double amount)
{
	struct device *d;

	list_for_each(d, &w->device_list, node) {
		double accel, old_accel;
		enum libinput_config_status status;

		if (!libinput_device_config_accel_is_available(d->dev))
			continue;

		accel = libinput_device_config_accel_get_speed(d->dev);
		if (fabs(accel + amount) > 1.0)
			continue;

		old_accel = accel;

		do {
			accel = clip(accel + amount, -1, 1);
			amount += amount;

			status = libinput_device_config_accel_set_speed(d->dev, accel);
			accel = libinput_device_config_accel_get_speed(d->dev);
		} while (status == LIBINPUT_CONFIG_STATUS_SUCCESS &&
			 accel == old_accel);

		if (status != LIBINPUT_CONFIG_STATUS_SUCCESS) {
			msg("%s: failed to change accel to %.2f (%s)\n",
			    libinput_device_get_sysname(d->dev),
			    accel,
			    libinput_config_status_to_str(status));
		}

	}
}


static void
handle_event_device_notify(struct libinput_event *ev)
{
	struct libinput_device *dev = libinput_event_get_device(ev);
	struct libinput *li;
	struct window *w;
	struct device *d;
	const char *type;

	if (libinput_event_get_type(ev) == LIBINPUT_EVENT_DEVICE_ADDED)
		type = "added";
	else
		type = "removed";

	msg("%s %s\n", libinput_device_get_sysname(dev), type);

	li = libinput_event_get_context(ev);
	w = libinput_get_user_data(li);

	if (libinput_event_get_type(ev) == LIBINPUT_EVENT_DEVICE_ADDED) {
		d = malloc(sizeof(*d));
		assert(d != NULL);

		d->dev = libinput_device_ref(dev);
		list_insert(&w->device_list, &d->node);
	} else  {
		list_for_each(d, &w->device_list, node) {
			if (d->dev == dev) {
				device_remove(d);
				break;
			}
		}
	}
}

static void
handle_event_motion(struct libinput_event *ev, struct window *w)
{
	struct libinput_event_pointer *p = libinput_event_get_pointer_event(ev);
	double dx = libinput_event_pointer_get_dx(p),
	       dy = libinput_event_pointer_get_dy(p);

	w->x += dx;
	w->y += dy;
	w->x = clip(w->x, 0.0, w->width);
	w->y = clip(w->y, 0.0, w->height);
}

static void
handle_event_absmotion(struct libinput_event *ev, struct window *w)
{
	struct libinput_event_pointer *p = libinput_event_get_pointer_event(ev);
	double x = libinput_event_pointer_get_absolute_x_transformed(p, w->width),
	       y = libinput_event_pointer_get_absolute_y_transformed(p, w->height);

	w->absx = x;
	w->absy = y;
}

static void
handle_event_touch(struct libinput_event *ev, struct window *w)
{
	struct libinput_event_touch *t = libinput_event_get_touch_event(ev);
	int slot = libinput_event_touch_get_seat_slot(t);
	struct touch *touch;
	double x, y;

	if (slot == -1 || slot >= ARRAY_LENGTH(w->touches))
		return;

	touch = &w->touches[slot];

	if (libinput_event_get_type(ev) == LIBINPUT_EVENT_TOUCH_UP) {
		touch->active = 0;
		return;
	}

	x = libinput_event_touch_get_x_transformed(t, w->width),
	y = libinput_event_touch_get_y_transformed(t, w->height);

	touch->active = 1;
	touch->x = (int)x;
	touch->y = (int)y;
}

static void
handle_event_axis(struct libinput_event *ev, struct window *w)
{
	struct libinput_event_pointer *p = libinput_event_get_pointer_event(ev);
	enum libinput_pointer_axis axis = libinput_event_pointer_get_axis(p);
	double v = libinput_event_pointer_get_axis_value(p);

	switch (axis) {
	case LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL:
		w->vy += (int)v;
		w->vy = clip(w->vy, 0, w->height);
		break;
	case LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL:
		w->hx += (int)v;
		w->hx = clip(w->hx, 0, w->width);
		break;
	default:
		abort();
	}
}

static int
handle_event_keyboard(struct libinput_event *ev, struct window *w)
{
	struct libinput_event_keyboard *k = libinput_event_get_keyboard_event(ev);
	unsigned int key = libinput_event_keyboard_get_key(k);

	if (libinput_event_keyboard_get_key_state(k) ==
	    LIBINPUT_KEY_STATE_RELEASED)
		return 0;

	switch(key) {
	case KEY_ESC:
		return 1;
	case KEY_UP:
		change_ptraccel(w, 0.1);
		break;
	case KEY_DOWN:
		change_ptraccel(w, -0.1);
		break;
	default:
		break;
	}

	return 0;
}

static bool
study_click_in_circle(struct window *w, int x, int y)
{
	struct study *s = &w->base;
	double dist;

	if (x < s->object_x - s->object_radius ||
	    x > s->object_x + s->object_radius ||
	    y < s->object_y - s->object_radius ||
	    y > s->object_y + s->object_radius)
		return false;

	dist = (x - s->object_x) * (x - s->object_x) +
		(y - s->object_y) * (y - s->object_y);

	if (dist > s->object_radius * s->object_radius)
		return false;

	return true;
}

static void
study_new_training_target(struct window *w)
{
	struct study *s = &w->base;
	int r;

	int point_dist = 300;

	int xoff = w->width/2 - point_dist * 1.5;
	int yoff = w->height/2 - point_dist;

	/* Grid of 4x3 positions */
	do {
		r = rand() % 12;
	} while(r == s->last_random);

	s->last_random = r;

	s->object_x = xoff + (r % 4) * point_dist;
	s->object_y = yoff + (r/4) * point_dist;
	s->ntargets--;
}

static void
study_show_start_target(struct window *w)
{
	struct study *s = &w->base;

	w->base.object_x = w->width/2;
	w->base.object_y = w->height/2;
	w->base.object_radius = s->radii[s->set];
}

static void
study_new_target(struct window *w)
{
	struct study *s = &w->base;
	struct timespec tp;
	uint32_t time;

	study_new_training_target(w);

	clock_gettime(CLOCK_MONOTONIC, &tp);
	time = tp.tv_sec * 1000 + tp.tv_nsec/1000000;

	dprintf(s->fd,
		"<target time=\"%d\" number=\"%d\" xpos=\"%d\" ypos=\"%d\" r=\"%d\" />\n",
		time,
		NUM_STUDY_TARGETS - s->ntargets,
		s->object_x,
		s->object_y,
		s->object_radius);
}

static void
study_mark_set_start(struct window *w)
{
	struct study *s = &w->base;
	struct timespec tp;
	uint32_t time;

	s->object_radius = s->radii[s->set];

	clock_gettime(CLOCK_MONOTONIC, &tp);
	time = tp.tv_sec * 1000 + tp.tv_nsec/1000000;

	dprintf(s->fd,
		"<set time=\"%d\" id=\"%d\" r=\"%d\">\n",
		time,
		s->set,
		s->object_radius);
}

static void
study_mark_set_stop(struct window *w)
{
	struct study *s = &w->base;

	dprintf(s->fd, "</set>\n");
}

static void
study_print_dmi_data(struct study *s)
{
	int fd, rc;
	const char *dmi_path = "/sys/devices/virtual/dmi/id/modalias";
	char buf[PATH_MAX] = {0};

	fd = open(dmi_path, O_RDONLY);
	if (fd == -1)
		return;

	rc = read(fd, buf, sizeof(buf) - 1);
	if (rc > 0)
		dprintf(s->fd, "%s", buf); /* data includes linebreak */

	close(fd);
}

static void
study_start_recording(struct window *w)
{
	struct study *s = &w->base;
	struct libevdev *evdev;
	int fd;
	int code, type;
	char path[PATH_MAX];
	struct utsname kernel;

	s->filename = strdup("userstudy-results.xml.XXXXXX");
	s->fd = mkstemp(s->filename);
	assert(s->fd > -1);
	s->cwd = get_current_dir_name();

	dprintf(s->fd, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
	dprintf(s->fd, "<results>\n");
	dprintf(s->fd, "<system>\n");

	/* kernel version */
	uname(&kernel);
	dprintf(s->fd,
		"<kernel name=\"%s\" release=\"%s\"/>\n",
		kernel.sysname,
		kernel.release);


	/* DMI data */
	dprintf(s->fd, "<dmi>\n");
	study_print_dmi_data(s);
	dprintf(s->fd, "</dmi>\n");
	dprintf(s->fd, "</system>\n");

	/* device info */
	dprintf(s->fd, "<device name=\"%s\" pid=\"%#x\" vid=\"%#x\">\n",
		libinput_device_get_name(s->device),
		libinput_device_get_id_product(s->device),
		libinput_device_get_id_vendor(s->device));

	snprintf(path,
		 sizeof(path),
		 "/dev/input/%s",
		 libinput_device_get_sysname(s->device));
	fd = open(path, O_RDONLY);
	assert(fd > 0);

	assert(libevdev_new_from_fd(fd, &evdev) == 0);

	for (type = EV_KEY; type < EV_MAX; type++) {
		int max = libevdev_event_type_get_max(type);

		if (!libevdev_has_event_type(evdev, type))
			continue;

		for (code = 0; code < max; code++) {
			if (!libevdev_has_event_code(evdev, type, code))
				continue;

			dprintf(s->fd,
				"<bit type=\"%d\" code=\"%d\"/> <!-- %s %s -->\n",
				type, code,
				libevdev_event_type_get_name(type),
				libevdev_event_code_get_name(type, code));
		}
	}

	libevdev_free(evdev);
	close(fd);

	dprintf(s->fd, "</device>\n");
	dprintf(s->fd, "<sets>\n");

	study_mark_set_start(w);
}

static void
study_stop_recording(struct window *w)
{
	struct study *s = &w->base;
	dprintf(s->fd, "</set>\n");
	dprintf(s->fd, "</sets>\n");
	dprintf(s->fd, "</results>\n");
	close(s->fd);
}

static void
study_record_event(struct window *w, struct libinput_event *ev)
{
	struct study *s = &w->base;
	struct libinput_device *device;
	struct libinput_event_pointer *ptrev;
	enum libinput_event_type type;

	if (s->state != STATE_STUDY)
		return;

	device = libinput_event_get_device(ev);
	if (device != s->device)
		return;

	type = libinput_event_get_type(ev);
	switch (type) {
	case LIBINPUT_EVENT_NONE:
		abort();
	case LIBINPUT_EVENT_DEVICE_ADDED:
	case LIBINPUT_EVENT_DEVICE_REMOVED:
	case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
	case LIBINPUT_EVENT_TOUCH_DOWN:
	case LIBINPUT_EVENT_TOUCH_MOTION:
	case LIBINPUT_EVENT_TOUCH_UP:
	case LIBINPUT_EVENT_POINTER_AXIS:
	case LIBINPUT_EVENT_TOUCH_CANCEL:
	case LIBINPUT_EVENT_TOUCH_FRAME:
	case LIBINPUT_EVENT_KEYBOARD_KEY:
		return;
	case LIBINPUT_EVENT_POINTER_MOTION:
	case LIBINPUT_EVENT_POINTER_BUTTON:
		break;
	}

	ptrev = libinput_event_get_pointer_event(ev);

	if (type == LIBINPUT_EVENT_POINTER_BUTTON) {
		enum libinput_button_state state =
			libinput_event_pointer_get_button_state(ptrev);
		dprintf(s->fd,
			"<button time=\"%d\" x=\"%f\" y=\"%f\" button=\"%d\" state=\"%d\"",
			libinput_event_pointer_get_time(ptrev),
			w->x, w->y,
			libinput_event_pointer_get_button(ptrev),
			state);
		if (state == LIBINPUT_BUTTON_STATE_PRESSED)
			dprintf(s->fd,
				" hit=\"%d\"",
				(int)study_click_in_circle(w, w->x, w->y));
		dprintf(s->fd, "/>\n");
	} else {
		dprintf(s->fd,
			"<motion time=\"%d\"  x=\"%f\" y=\"%f\" dx=\"%f\" dy=\"%f\"/>\n",
			libinput_event_pointer_get_time(ptrev),
			w->x, w->y,
			libinput_event_pointer_get_dx(ptrev),
			libinput_event_pointer_get_dy(ptrev));
	}
}

static void
study_handle_event_button(struct libinput_event *ev, struct window *w)
{
	struct study *s = &w->base;
	struct libinput_event_pointer *p = libinput_event_get_pointer_event(ev);
	int is_press;
	struct libinput_device *device = libinput_event_get_device(ev);

	is_press = libinput_event_pointer_get_button_state(p) == LIBINPUT_BUTTON_STATE_PRESSED;

	if (s->device && device != s->device)
		return;

	/* Drop the release event after confirming dialogs */
	if (!is_press) {
		if (s->new_state == STATE_STUDY &&
		    s->state != s->new_state) {
			s->state = s->new_state;
			study_new_target(w);
		}
		return;
	}

	switch(s->state) {
	case STATE_CONFIRM_DEVICE:
		if (!study_click_in_circle(w, w->x, w->y))
		    return;

		assert(s->device == NULL);
		s->device = libinput_event_get_device(ev);

		study_show_training_start(w);

		s->new_state = STATE_TRAINING;
		s->state = STATE_TRAINING;
		s->ntargets = NUM_TRAINING_TARGETS;
		study_default_target(w);
		break;
	case STATE_TRAINING:
		if (!study_click_in_circle(w, w->x, w->y))
			return;

		if (s->ntargets == 0) {
			study_show_training_done(w);
			s->new_state = STATE_STUDY_START;
			s->state = STATE_STUDY_START;
			study_show_start_target(w);
			break;
		}
		study_new_training_target(w);
		break;
	case STATE_STUDY_START:
		if (!study_click_in_circle(w, w->x, w->y))
			return;
		s->new_state = STATE_STUDY;
		s->ntargets = NUM_STUDY_TARGETS;
		study_start_recording(w);
		break;
	case STATE_INTERMISSION:
		if (!study_click_in_circle(w, w->x, w->y))
			return;
		s->new_state = STATE_STUDY;
		study_mark_set_start(w);
		s->ntargets = NUM_STUDY_TARGETS;
		break;
	case STATE_STUDY:
		if (!study_click_in_circle(w, w->x, w->y))
			return;

		if (s->ntargets == 0) {
			s->set++;
			if (s->set < NUM_SETS) {
				study_mark_set_stop(w);
				study_show_intermission(w);
				s->state = STATE_INTERMISSION;
				s->new_state = STATE_INTERMISSION;
				study_show_start_target(w);
				break;
			} else {
				s->state = STATE_DONE;
				s->new_state = STATE_DONE;
				study_stop_recording(w);
				study_show_done(w);
				gtk_main_quit();
				printf("Your results are in %s/%s\n",
				       s->cwd,
				       s->filename);
				return;
			}
		}
		study_new_target(w);
		break;
	default:
		return;
	}
}

static void
handle_event_button(struct libinput_event *ev, struct window *w)
{
	struct libinput_event_pointer *p = libinput_event_get_pointer_event(ev);
	unsigned int button = libinput_event_pointer_get_button(p);
	int is_press;

	is_press = libinput_event_pointer_get_button_state(p) == LIBINPUT_BUTTON_STATE_PRESSED;

	switch (button) {
	case BTN_LEFT:
		w->l = is_press;
		break;
	case BTN_RIGHT:
		w->r = is_press;
		break;
	case BTN_MIDDLE:
		w->m = is_press;
		break;
	}

	study_handle_event_button(ev, w);
}

static gboolean
handle_event_libinput(GIOChannel *source, GIOCondition condition, gpointer data)
{
	struct libinput *li = data;
	struct window *w = libinput_get_user_data(li);
	struct libinput_event *ev;

	libinput_dispatch(li);

	while ((ev = libinput_get_event(li))) {
		study_record_event(w, ev);

		switch (libinput_event_get_type(ev)) {
		case LIBINPUT_EVENT_NONE:
			abort();
		case LIBINPUT_EVENT_DEVICE_ADDED:
		case LIBINPUT_EVENT_DEVICE_REMOVED:
			handle_event_device_notify(ev);
			break;
		case LIBINPUT_EVENT_POINTER_MOTION:
			handle_event_motion(ev, w);
			break;
		case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
			handle_event_absmotion(ev, w);
			break;
		case LIBINPUT_EVENT_TOUCH_DOWN:
		case LIBINPUT_EVENT_TOUCH_MOTION:
		case LIBINPUT_EVENT_TOUCH_UP:
			handle_event_touch(ev, w);
			break;
		case LIBINPUT_EVENT_POINTER_AXIS:
			handle_event_axis(ev, w);
			break;
		case LIBINPUT_EVENT_TOUCH_CANCEL:
		case LIBINPUT_EVENT_TOUCH_FRAME:
			break;
		case LIBINPUT_EVENT_POINTER_BUTTON:
			handle_event_button(ev, w);
			break;
		case LIBINPUT_EVENT_KEYBOARD_KEY:
			if (handle_event_keyboard(ev, w)) {
				libinput_event_destroy(ev);
				gtk_main_quit();
				return FALSE;
			}
			break;
		}

		libinput_event_destroy(ev);
		libinput_dispatch(li);
	}
	gtk_widget_queue_draw(w->area);

	return TRUE;
}

static int
check_for_devices(struct libinput *li)
{
	libinput_dispatch(li);

	/* we expect all DEVICE_ADDED events before any other events */
	if (libinput_next_event_type(li) ==
	    LIBINPUT_EVENT_DEVICE_ADDED)
		return 0;

	return 1;
}

static void
sockets_init(struct libinput *li)
{
	GIOChannel *c = g_io_channel_unix_new(libinput_get_fd(li));

	g_io_channel_set_encoding(c, NULL, NULL);
	g_io_add_watch(c, G_IO_IN, handle_event_libinput, li);
}

static int
parse_opts(int argc, char *argv[])
{
	while (1) {
		static struct option long_options[] = {
			{ "help", no_argument, 0, 'h' },
		};

		int option_index = 0;
		int c;

		c = getopt_long(argc, argv, "h", long_options,
				&option_index);
		if (c == -1)
			break;

		switch(c) {
		case 'h':
			usage();
			return 0;
		default:
			usage();
			return 1;
		}
	}

	return 0;
}


static int
open_restricted(const char *path, int flags, void *user_data)
{
	int fd = open(path, flags);
	return fd < 0 ? -errno : fd;
}

static void
close_restricted(int fd, void *user_data)
{
	close(fd);
}

const static struct libinput_interface interface = {
	.open_restricted = open_restricted,
	.close_restricted = close_restricted,
};

int
main(int argc, char *argv[])
{
	struct window w;
	struct libinput *li;
	struct udev *udev;

	gtk_init(&argc, &argv);

	if (parse_opts(argc, argv) != 0)
		return 1;

	udev = udev_new();
	if (!udev)
		error("Failed to initialize udev\n");

	li = libinput_udev_create_context(&interface, &w, udev);
	if (!li || libinput_udev_assign_seat(li, "seat0") != 0)
		error("Failed to initialize context from udev\n");

	if (check_for_devices(li) != 0) {
		fprintf(stderr, "Unable to find at least one input device.\n\n");
		usage_device();
		return 1;
	}

	window_init(&w);
	study_init(&w);
	sockets_init(li);

	gtk_main();

	window_cleanup(&w);
	study_cleanup(&w);
	libinput_unref(li);
	udev_unref(udev);

	return 0;
}
