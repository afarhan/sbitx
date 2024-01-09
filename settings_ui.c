/* 
	By Ashhar Farhan and chatGPT.
	Under GPL v3

	This code was generated entirely with asking the chatgpt this:

	write a gtk application that shows a dialog box with the following fields:
	1. A text field with the label "Callsign" with a minimum text length of 3 and a maximum of 11 that is all caps.
	2. A text field with the label "My Grid" with a minium text length of 4 and a maximum of 6 that is all caps
	3. A number field PIN with mimum 3 digits and maximum 6 digits
	4. A check box with the label "Report"

At first it wrote it in python as I had not specified C, then I asked:
	can you write this in C?

	To this I added the sbitx headers and the code to get and set the current
	settings values

*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h> 
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include <unistd.h>
#include <wiringPi.h>
#include <wiringSerial.h>
#include <linux/types.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
#include <sys/types.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <gtk/gtk.h>
#include "sdr.h"
#include "sdr_ui.h"
#include "sound.h"
#include "modem_ft8.h"
#include "modem_cw.h"


// Function to handle the OK button click event
static void ok_button_clicked(GtkWidget *widget, gpointer data) {
    // Retrieve data from entries
    const gchar *callsign = gtk_entry_get_text(GTK_ENTRY(data));
    const gchar *my_grid = gtk_entry_get_text(GTK_ENTRY(data + 1));
    const gchar *pin = gtk_entry_get_text(GTK_ENTRY(data + 2));
    gboolean report_checked = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data + 3));

    // You can perform operations with the retrieved data here
    // For example, print them to the console
		field_set("MYCALLSIGN", callsign);
		field_set("MYGRID", my_grid);
		field_set("PASSKEY", pin);
		
    g_print("Callsign: %s\n", callsign);
    g_print("My Grid: %s\n", my_grid);
    g_print("PIN: %s\n", pin);
    g_print("Report Checked: %s\n", report_checked ? "True" : "False");
}

// Signal handler to allow only uppercase letters and numbers for Callsign
static void on_callsign_changed(GtkWidget *widget, gpointer data) {
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(widget));
    gchar *result = g_strdup(text);

    for (int i = 0; i < strlen(text); ++i) {
        if (!isalnum(text[i])) {
            result[i] = '\0';
        } else {
            result[i] = toupper(text[i]);
        }
    }

    gtk_entry_set_text(GTK_ENTRY(widget), result);
    g_free(result);
}

// Signal handler to allow only uppercase letters and numbers for My Grid
static void on_my_grid_changed(GtkWidget *widget, gpointer data) {
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(widget));
    gchar *result = g_strdup(text);

    for (int i = 0; i < strlen(text); ++i) {
        if (!isalnum(text[i])) {
            result[i] = '\0';
        } else {
            result[i] = toupper(text[i]);
        }
    }

    gtk_entry_set_text(GTK_ENTRY(widget), result);
    g_free(result);
}


// Function to create the dialog box
void settings_ui(GtkWidget* parent){
    GtkWidget *dialog, *grid, *label, *entry_callsign, *entry_grid, *entry_pin, *check_button;
    GtkWidget *ok_button, *cancel_button;

    dialog = gtk_dialog_new_with_buttons("Settings", GTK_WINDOW(parent),
    	GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      "OK", GTK_RESPONSE_ACCEPT,
      "Cancel", GTK_RESPONSE_CANCEL,
       NULL);
/*
    // Create a dialog window
    dialog = gtk_dialog_new_with_buttons("Dialog Example",
                                         NULL,
                                         GTK_DIALOG_MODAL,
                                         "_Cancel",
                                         GTK_RESPONSE_CANCEL,
                                         "_OK",
                                         GTK_RESPONSE_OK,
                                         NULL);
*/
    // Create a grid for organizing the dialog's contents
    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 30);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), grid, TRUE, TRUE, 0);

    // Callsign field
    label = gtk_label_new("Callsign");
    gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 1, 1);
    entry_callsign = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(entry_callsign), 11);
    g_signal_connect(entry_callsign, "changed", G_CALLBACK(on_callsign_changed), NULL); // Connect signal handler
    gtk_grid_attach(GTK_GRID(grid), entry_callsign, 1, 0, 1, 1);
		gtk_entry_set_text(GTK_ENTRY(entry_callsign), (gchar *)field_str("MYCALLSIGN"));

    // My Grid field
    label = gtk_label_new("My Grid");
    gtk_grid_attach(GTK_GRID(grid), label, 0, 1, 1, 1);
    entry_grid = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(entry_grid), 6);
    g_signal_connect(entry_grid, "changed", G_CALLBACK(on_my_grid_changed), NULL); // Connect signal handler
    gtk_grid_attach(GTK_GRID(grid), entry_grid, 1, 1, 1, 1);
		gtk_entry_set_text(GTK_ENTRY(entry_grid), (gchar *)field_str("MYGRID"));

    // PIN field
    label = gtk_label_new("PIN");
    gtk_grid_attach(GTK_GRID(grid), label, 0, 2, 1, 1);
    entry_pin = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(entry_pin), 6);
    gtk_entry_set_input_purpose(GTK_ENTRY(entry_pin), GTK_INPUT_PURPOSE_NUMBER);
    gtk_grid_attach(GTK_GRID(grid), entry_pin, 1, 2, 1, 1);
		gtk_entry_set_text(GTK_ENTRY(entry_pin), field_str("PASSKEY"));

/*
    // Report checkbox
    check_button = gtk_check_button_new_with_label("Report");
    gtk_grid_attach(GTK_GRID(grid), check_button, 0, 3, 2, 1);
*/


    // Connect OK button signal to the handler function
    ok_button = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
//    g_signal_connect(ok_button, "clicked", G_CALLBACK(ok_button_clicked), entry);


    gtk_widget_show_all(dialog);
    //gtk_dialog_run(GTK_DIALOG(dialog));
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));

    if (response == GTK_RESPONSE_ACCEPT) {
        const gchar *callsign = gtk_entry_get_text(GTK_ENTRY(entry_callsign));
        const gchar *pin = gtk_entry_get_text(GTK_ENTRY(entry_pin));
        const gchar *grid = gtk_entry_get_text(GTK_ENTRY(entry_grid));

    // You can perform operations with the retrieved data here
    // For example, print them to the console
		field_set("MYCALLSIGN", callsign);
		field_set("MYGRID", grid);
		field_set("PASSKEY", pin);
		
				printf("%s, %s, %s\n", callsign, grid, pin);
        // Do something with the entered data (e.g., add it to the list)
        // Example:
        //add_to_list(list_store, user_id, password, grid_settings, "", "", "", "");
    }

    gtk_widget_destroy(dialog);
}
