// fast_js.c
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <gui/modules/text_input.h>
#include <gui/modules/variable_item_list.h>
#include <dialogs/dialogs.h>
#include <storage/storage.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <js_thread.h>
#include <toolbox/path.h>
#include "js_app_i.h"
#include <cli/cli.h>
#include "js_modules.h"

#define TAG "FastJS"

// Define ViewEvent as an alias for uint32_t
typedef uint32_t ViewEvent;

// Define a custom event ID for file browser
#define VIEW_EVENT_FILE_BROWSER 1

// Define the submenu items for our FastJS application
typedef enum
{
    FastJSSubmenuIndexRun,    // The main screen
    FastJSSubmenuIndexAbout,  // The about screen
    FastJSSubmenuIndexConfig, // The configuration screen
} FastJSSubmenuIndex;

// Define views for our FastJS application
typedef enum
{
    FastJSViewMain,      // The main screen
    FastJSViewSubmenu,   // The menu when the app starts
    FastJSViewAbout,     // The about screen
    FastJSViewConfigure, // The configuration screen
    FastJSViewConsole,   // Console output view
} FastJSView;

// Define a structure to hold settings
typedef struct
{
    bool remember_script;
    char script_path[256]; // Adjust the size as needed
} SettingsData;

// Forward declaration of FastJSApp for use in FastJSModel
typedef struct FastJSApp FastJSApp;

// Define the application structure
struct FastJSApp
{
    ViewDispatcher *view_dispatcher;             // Switches between our views
    Submenu *submenu;                            // The application submenu
    Widget *widget_about;                        // The about screen
    VariableItemList *variable_item_list_config; // The configuration screen
    JsConsoleView *console_view;                 // Console output view

    VariableItem *remember_item;        // Reference to the remember configuration item
    VariableItem *javascript_file_item; // Reference to the script file configuration item

    bool remember_script;           // Whether to remember the script
    char *selected_javascript_file; // Store the selected script file path
    char *temp_buffer;              // Temporary buffer
    uint32_t temp_buffer_size;      // Size of the temporary buffer

    JsThread *js_thread; // JavaScript execution thread
    Gui *gui;            // GUI reference
};

// Path to save the remembered script file path
#define REMEMBERED_SCRIPT_PATH STORAGE_EXT_PATH_PREFIX "/apps_data/fast_js_app/settings.bin"

static void save_settings(bool remember_script, const char *script_path)
{
    // Create the directory for saving settings
    char directory_path[256];
    snprintf(directory_path, sizeof(directory_path), STORAGE_EXT_PATH_PREFIX "/apps_data/fast_js_app");

    // Create the directory
    Storage *storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, directory_path);

    // Open the settings file
    File *file = storage_file_alloc(storage);
    if (!storage_file_open(file, REMEMBERED_SCRIPT_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS))
    {
        FURI_LOG_E(TAG, "Failed to open settings file for writing: %s", REMEMBERED_SCRIPT_PATH);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return;
    }

    // Write the remember_script as a single byte
    uint8_t remember_script_byte = remember_script ? 1 : 0;
    if (storage_file_write(file, &remember_script_byte, sizeof(uint8_t)) != sizeof(uint8_t))
    {
        FURI_LOG_E(TAG, "Failed to write remember_script");
    }

    // Write the script_path as a null-terminated string
    size_t script_path_length = strlen(script_path) + 1; // Include null terminator
    if (storage_file_write(file, script_path, script_path_length) != script_path_length)
    {
        FURI_LOG_E(TAG, "Failed to write script_path");
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

// In load_settings
static bool load_settings(bool *remember_script, char *buffer, size_t buffer_size)
{
    Storage *storage = furi_record_open(RECORD_STORAGE);
    File *file = storage_file_alloc(storage);

    if (!storage_file_open(file, REMEMBERED_SCRIPT_PATH, FSAM_READ, FSOM_OPEN_EXISTING))
    {
        FURI_LOG_E(TAG, "Failed to open settings file for reading: %s", REMEMBERED_SCRIPT_PATH);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false; // Return false if the file does not exist
    }

    // Read the remember_script as a single byte
    uint8_t remember_script_byte;
    if (storage_file_read(file, &remember_script_byte, sizeof(uint8_t)) != sizeof(uint8_t))
    {
        FURI_LOG_E(TAG, "Failed to read remember_script");
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }
    *remember_script = remember_script_byte ? true : false;

    // Read the script_path as a null-terminated string
    ssize_t read_size = storage_file_read(file, buffer, buffer_size - 1);
    if (read_size <= 0)
    {
        FURI_LOG_E(TAG, "Failed to read script_path");
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }
    buffer[read_size] = '\0'; // Ensure null-termination

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return true;
}

// In fast_js_navigation_configure_callback
static uint32_t fast_js_navigation_configure_callback(void *context)
{
    UNUSED(context);
    return FastJSViewSubmenu;
}

// Navigation callback for the About screen to go back to the submenu
static uint32_t fast_js_navigation_about_callback(void *context)
{
    UNUSED(context);
    return FastJSViewSubmenu;
}

// Navigation callback for exiting the application from the submenu
static uint32_t fast_js_submenu_exit_callback(void *context)
{
    // Exit the application
    UNUSED(context);
    return VIEW_NONE; // Return VIEW_NONE to exit the app
}

// Callback function for JS thread events
static void js_callback(JsThreadEvent event, const char *msg, void *context)
{
    FastJSApp *app = (FastJSApp *)context;
    furi_assert(app);

    if (event == JsThreadEventDone)
    {
        FURI_LOG_I(TAG, "Script done");
        console_view_print(app->console_view, "--- DONE ---");
    }
    else if (event == JsThreadEventPrint)
    {
        console_view_print(app->console_view, msg);
    }
    else if (event == JsThreadEventError)
    {
        console_view_print(app->console_view, "--- ERROR ---");
        console_view_print(app->console_view, msg);
    }
    else if (event == JsThreadEventErrorTrace)
    {
        FuriString *compact_trace = furi_string_alloc_set_str(msg);
        // Compact the trace message
        // Keep only first line
        size_t line_end = furi_string_search_char(compact_trace, '\n');
        if (line_end > 0)
        {
            furi_string_left(compact_trace, line_end);
        }

        // Remove full path
        FuriString *file_name = furi_string_alloc();
        size_t filename_start = furi_string_search_rchar(compact_trace, '/');
        if (filename_start > 0)
        {
            filename_start++;
            furi_string_set_n(
                file_name, compact_trace, filename_start, furi_string_size(compact_trace) - filename_start);
            furi_string_printf(compact_trace, "at %s", furi_string_get_cstr(file_name));
        }

        furi_string_free(file_name);

        console_view_print(app->console_view, furi_string_get_cstr(compact_trace));
        furi_string_free(compact_trace);
        console_view_print(app->console_view, "See logs for full trace");
    }
}

// Navigation callback for the Console view to go back to the submenu
static uint32_t fast_js_navigation_console_callback(void *context)
{
    FastJSApp *app = (FastJSApp *)context;
    // Stop the JS thread if it's still running
    if (app->js_thread)
    {
        js_thread_stop(app->js_thread);
        app->js_thread = NULL;
    }
    return FastJSViewSubmenu;
}

// Custom event callback to handle file browser dialog
static bool fast_js_file_browser_callback(void *context, uint32_t event)
{
    if (event == VIEW_EVENT_FILE_BROWSER)
    {
        FastJSApp *app = (FastJSApp *)context;

        DialogsApp *dialogs = furi_record_open(RECORD_DIALOGS);
        if (!dialogs)
        {
            return false;
        }

        DialogsFileBrowserOptions browser_options;

        browser_options.extension = "js";
        browser_options.base_path = STORAGE_APP_DATA_PATH_PREFIX;
        browser_options.skip_assets = true;
        browser_options.hide_dot_files = true;
        browser_options.icon = NULL;
        browser_options.hide_ext = false;

        // Show the file browser dialog at the Scripts directory
        FuriString *javascript_file_path = furi_string_alloc_set_str("/ext/apps/Scripts");
        if (!javascript_file_path)
        {
            furi_record_close(RECORD_DIALOGS);
            return false;
        }

        if (dialog_file_browser_show(dialogs, javascript_file_path, javascript_file_path, &browser_options))
        {
            // Store the selected script file path
            const char *file_path = furi_string_get_cstr(javascript_file_path);
            strncpy(app->selected_javascript_file, file_path, app->temp_buffer_size);
            app->selected_javascript_file[app->temp_buffer_size - 1] = '\0'; // Ensure null-termination

            // Update the script file item text
            if (app->javascript_file_item)
            {
                variable_item_set_current_value_text(app->javascript_file_item, app->selected_javascript_file);
            }

            // Save the settings if "Remember" is YES
            if (app->remember_script)
            {
                save_settings(app->remember_script, app->selected_javascript_file);
            }
            else
            {
                // Save the settings only if "Remember" is NO
                save_settings(app->remember_script, "");
            }
        }

        furi_string_free(javascript_file_path);
        furi_record_close(RECORD_DIALOGS);

        // Return to the configuration view
        view_dispatcher_switch_to_view(app->view_dispatcher, FastJSViewConfigure);

        return true; // Event handled
    }

    return false; // Event not handled
}

// Handle submenu item selection
static void fast_js_submenu_callback(void *context, uint32_t index)
{
    FastJSApp *app = (FastJSApp *)context;
    switch (index)
    {
    case FastJSSubmenuIndexRun:
        // Execute the script
        if (app->selected_javascript_file[0] == '\0')
        {
            console_view_print(app->console_view, "No script file selected.");
        }
        else
        {
            // Switch to the console view to execute the script file
            view_dispatcher_switch_to_view(app->view_dispatcher, FastJSViewConsole);
            // Start executing the script
            FuriString *name = furi_string_alloc();
            FuriString *script_path = furi_string_alloc_set(app->selected_javascript_file);
            path_extract_filename(script_path, name, false);
            FuriString *start_text = furi_string_alloc_printf("Running %s", furi_string_get_cstr(name));
            console_view_print(app->console_view, furi_string_get_cstr(start_text));
            console_view_print(app->console_view, "------------");
            furi_string_free(name);
            furi_string_free(start_text);

            app->js_thread = js_thread_run(furi_string_get_cstr(script_path), js_callback, app);
            furi_string_free(script_path);
        }
        break;
    case FastJSSubmenuIndexAbout:
        view_dispatcher_switch_to_view(app->view_dispatcher, FastJSViewAbout);
        break;
    case FastJSSubmenuIndexConfig:
        view_dispatcher_switch_to_view(app->view_dispatcher, FastJSViewConfigure);
        break;
    default:
        break;
    }
}

// Modify the callback for when the Remember option is toggled
static void fast_js_config_item_selected(void *context, uint32_t index)
{
    FastJSApp *app = (FastJSApp *)context;
    // In fast_js_config_item_selected
    if (index == 0)
    {
        // Toggle Remember option
        app->remember_script = !app->remember_script;
        const char *remember_text = app->remember_script ? "YES" : "NO";
        variable_item_set_current_value_text(app->remember_item, remember_text);

        // Save the settings each time Remember option is changed
        if (app->remember_script)
        {
            save_settings(app->remember_script, app->selected_javascript_file);
        }
        else
        {
            save_settings(app->remember_script, "");
        }
    }
    else if (index == 1)
    {
        // Script file selection
        view_dispatcher_send_custom_event(app->view_dispatcher, VIEW_EVENT_FILE_BROWSER);
    }
}

static FastJSApp *fast_js_app_alloc()
{
    FastJSApp *app = (FastJSApp *)malloc(sizeof(FastJSApp));
    if (!app)
    {
        return NULL;
    }

    app->temp_buffer_size = 256; // Increased buffer size for longer paths
    app->temp_buffer = (char *)malloc(app->temp_buffer_size);
    app->selected_javascript_file = (char *)malloc(app->temp_buffer_size);

    if (!app->temp_buffer || !app->selected_javascript_file)
    {
        free(app->temp_buffer);
        free(app->selected_javascript_file);
        free(app);
        return NULL;
    }

    // Initialize buffers with empty strings
    app->temp_buffer[0] = '\0';
    app->selected_javascript_file[0] = '\0';
    app->remember_script = false;

    // Try to load the remembered settings
    // In fast_js_app_alloc
    if (!load_settings(&app->remember_script, app->selected_javascript_file, app->temp_buffer_size))
    {
        FURI_LOG_I(TAG, "No saved settings found; using defaults");
    }

    app->view_dispatcher = view_dispatcher_alloc();
    if (!app->view_dispatcher)
    {
        free(app->temp_buffer);
        free(app->selected_javascript_file);
        free(app);
        return NULL;
    }

    app->gui = furi_record_open(RECORD_GUI);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);

    // Set the custom event callback
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, fast_js_file_browser_callback);

    // Console view
    app->console_view = console_view_alloc();
    view_dispatcher_add_view(app->view_dispatcher, FastJSViewConsole, console_view_get_view(app->console_view));
    view_set_previous_callback(console_view_get_view(app->console_view), fast_js_navigation_console_callback);
    view_set_context(console_view_get_view(app->console_view), app);

    // Submenu view
    app->submenu = submenu_alloc();
    submenu_add_item(app->submenu, "Run", FastJSSubmenuIndexRun, fast_js_submenu_callback, app);
    submenu_add_item(app->submenu, "About", FastJSSubmenuIndexAbout, fast_js_submenu_callback, app);
    submenu_add_item(app->submenu, "Configure", FastJSSubmenuIndexConfig, fast_js_submenu_callback, app);
    view_set_previous_callback(submenu_get_view(app->submenu), fast_js_submenu_exit_callback);
    view_dispatcher_add_view(app->view_dispatcher, FastJSViewSubmenu, submenu_get_view(app->submenu));

    // Configuration view
    app->variable_item_list_config = variable_item_list_alloc();
    app->remember_item = variable_item_list_add(app->variable_item_list_config, "Remember", 1, NULL, NULL);
    const char *remember_text = app->remember_script ? "YES" : "NO";
    variable_item_set_current_value_text(app->remember_item, remember_text);
    app->javascript_file_item = variable_item_list_add(app->variable_item_list_config, "Select Script File", 1, NULL, NULL);
    if (app->selected_javascript_file[0] != '\0')
    {
        variable_item_set_current_value_text(app->javascript_file_item, app->selected_javascript_file);
    }
    else
    {
        variable_item_set_current_value_text(app->javascript_file_item, "Not selected");
    }

    variable_item_list_set_enter_callback(app->variable_item_list_config, fast_js_config_item_selected, app);
    view_set_previous_callback(variable_item_list_get_view(app->variable_item_list_config), fast_js_navigation_configure_callback);
    view_dispatcher_add_view(app->view_dispatcher, FastJSViewConfigure, variable_item_list_get_view(app->variable_item_list_config));

    // About view
    app->widget_about = widget_alloc();
    widget_add_text_scroll_element(
        app->widget_about,
        0,
        0,
        128,
        64,
        "FastJS App\n---\nExecute your scripts seamlessly.\n---\nPress BACK to return.");
    view_set_previous_callback(widget_get_view(app->widget_about), fast_js_navigation_about_callback);
    view_dispatcher_add_view(app->view_dispatcher, FastJSViewAbout, widget_get_view(app->widget_about));

    // Start with the submenu view
    view_dispatcher_switch_to_view(app->view_dispatcher, FastJSViewSubmenu);

    return app;
}

// Function to free the resources used by FastJSApp
static void fast_js_app_free(FastJSApp *app)
{
    if (!app)
        return;

    // Stop JS thread if running
    if (app->js_thread)
    {
        js_thread_stop(app->js_thread);
        app->js_thread = NULL;
    }

    // Free console view
    view_dispatcher_remove_view(app->view_dispatcher, FastJSViewConsole);
    console_view_free(app->console_view);

    // Free submenu view
    view_dispatcher_remove_view(app->view_dispatcher, FastJSViewSubmenu);
    submenu_free(app->submenu);

    // Free configuration view
    view_dispatcher_remove_view(app->view_dispatcher, FastJSViewConfigure);
    variable_item_list_free(app->variable_item_list_config);

    // Free buffers
    free(app->temp_buffer);
    free(app->selected_javascript_file);

    // Free about view
    view_dispatcher_remove_view(app->view_dispatcher, FastJSViewAbout);
    widget_free(app->widget_about);

    // Free dispatcher
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);

    // Free the app structure
    free(app);
}

// Entry point for the FastJS application
int32_t fast_js_app(void *p)
{
    UNUSED(p);

    // Initialize the FastJS application
    FastJSApp *app = fast_js_app_alloc();
    if (!app)
    {
        // Allocation failed
        return -1; // Indicate failure
    }

    // Run the view dispatcher
    view_dispatcher_run(app->view_dispatcher);

    // Free the resources used by the FastJS application
    fast_js_app_free(app);

    // Return 0 to indicate success
    return 0;
}
