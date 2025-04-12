#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <dialogs/dialogs.h>
#include <storage/storage.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <printf_tiny.h>

// Function to open the file browser
bool open_file_browser(const char *path, const char *file_extension)
{
    // Set default file extension if not provided
    if (file_extension == NULL)
    {
        file_extension = ".js";
    }

    // Open the dialogs record
    DialogsApp *dialogs = furi_record_open(RECORD_DIALOGS);
    if (!dialogs)
    {
        return false;
    }

    // Set up file browser options
    DialogsFileBrowserOptions browser_options;
    dialog_file_browser_set_basic_options(&browser_options, file_extension, NULL);

    browser_options.extension = file_extension;
    browser_options.base_path = STORAGE_APP_DATA_PATH_PREFIX;
    browser_options.skip_assets = true;
    browser_options.hide_dot_files = true;
    browser_options.icon = NULL;
    browser_options.hide_ext = false;

    // Allocate memory for the file path
    FuriString *furi_file_path = furi_string_alloc_set_str(path);
    if (!furi_file_path)
    {
        furi_record_close(RECORD_DIALOGS);
        return false;
    }

    // Show the file browser dialog
    bool result = dialog_file_browser_show(dialogs, furi_file_path, furi_file_path, &browser_options);

    // Clean up
    furi_string_free(furi_file_path);
    furi_record_close(RECORD_DIALOGS);

    return result;
}

typdef struct
{
    const char *title;
    const char *items[];
    size_t item_count;
    void callbacks[];
    void callback_context;
} SubmenuData;

bool create_submenu(SubmenuData *data, SubmenuView *view, ViewDispatcher *dispatcher)
{
    // Allocate memory for the submenu
    Submenu *submenu = submenu_alloc();
    if (!submenu)
    {
        return false;
    }

    // Set the submenu header
    submenu_set_header(submenu, data->title);

    // Add items to the submenu
    for (size_t i = 0; i < data->item_count; ++i)
    {
        submenu_add_item(submenu, data->items[i], i, data->callbacks[i], data->callback_context);
    }

    // Add the submenu to the view dispatcher
    view_dispatcher_add_view(dispatcher, view, submenu_get_view(submenu));
    return true;
}