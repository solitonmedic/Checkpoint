/*
 * extdata.c — create the extdata archive a title does not have yet.
 *
 * The Extdata tab lists a title only when an extdata archive for it exists on
 * the SD card, and the archive is made by the game the first time it writes
 * extdata. A game that never does has none, so there is nothing to back up and
 * nothing to restore an extdata backup into. This creates that empty archive.
 *
 * The id matters: Checkpoint looks for one specific extdata id per title
 * (extdata_default_id), and an archive under any other id exists on the card
 * but is never listed. The custom-id path is for reading an archive written
 * under an id Checkpoint does not derive — check with "Inspect an extdata id"
 * before assuming.
 */

#include <checkpoint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEXN     9          /* 8 hex digits + terminator                        */
#define ID_MAX   0x7FFFFFFF /* the largest id the bindings take                 */
#define CAP_MAX  10000      /* the API's ceiling on folders / files             */
#define LIST_MAX 12         /* entries printed when inspecting an archive       */
#define MSGN     512        /* a message holding a title's long description     */

/* Parse an extdata id typed as hex, with or without a "0x". -1 if it is empty,
 * not hex, or bigger than the API takes. */
int parseHex(char* s)
{
    int v = 0;
    int i = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        i = 2;
    }
    if (s[i] == '\0') {
        return -1;
    }

    while (s[i] != '\0') {
        int c = s[i];
        int d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        }
        else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        }
        else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        }
        else {
            return -1;
        }
        if (v > ID_MAX / 16) {
            return -1;
        }
        v = v * 16 + d;
        i = i + 1;
    }
    return v;
}

/* Keyboard prompt for an extdata id. -1 on cancel or on anything unparseable. */
int askExtdataId(char* prompt)
{
    char text[HEXN];
    int id;

    text[0] = '\0';
    gui_keyboard(text, prompt, HEXN);
    if (text[0] == '\0') {
        return -1; /* cancelled */
    }

    id = parseHex(text);
    if (id < 0) {
        gui_message("That is not an extdata id.\nType it in hex, e.g. 1255 or 0x1255.");
    }
    return id;
}

/* Report what is in an archive's root, so the user can tell one that holds a
 * save from one that is not there at all. Returns the number of root entries,
 * -1 if there is no archive under that id. */
int describeExtdata(int extdataId)
{
    char line[128];
    int h;
    int count;
    struct directory* dir;
    int i;

    h = sav_open_extdata(extdataId);
    if (h < 0) {
        return -1;
    }

    dir = sav_list(h, "/");
    if (dir == NULL) {
        sav_close(h);
        return 0;
    }

    count = dir->count;
    sprintf(line, "extdata 0x%08X: %d entr(y/ies) in the root", extdataId, count);
    script_log(line);
    for (i = 0; i < count && i < LIST_MAX; i++) {
        printf("  %s\n", dir->files[i]);
    }
    if (count > LIST_MAX) {
        printf("  ... and %d more\n", count - LIST_MAX);
    }

    delete_directory(dir);
    sav_close(h);
    return count;
}

/* Turn extdata_create's return value into something the user can act on. */
void reportCreateResult(int res, char* name, int extdataId, int defaultId)
{
    char msg[MSGN];

    if (res == 0) {
        if (extdataId == defaultId) {
            sprintf(msg,
                "Extdata created for %s.\nHold B on the title list to refresh,\nthen open the extdata tab — the title is\nthere with an empty backup list.",
                name);
        }
        else {
            sprintf(msg,
                "Extdata created under id 0x%08X.\nThat is not the id Checkpoint looks for\n(0x%08X), so the title will NOT appear\nin the extdata tab.",
                extdataId, defaultId);
        }
        gui_message(msg);
        return;
    }

    if (res == -2) {
        sprintf(msg, "An extdata archive already exists under\nid 0x%08X. Nothing was changed —\nan existing archive is never recreated.", extdataId);
    }
    else if (res == -1) {
        sprintf(msg, "%s is not in Checkpoint's title list,\nso its icon could not be read and the\narchive was not created.", name);
    }
    else {
        sprintf(msg, "The console refused to create the archive\n(result %d). Nothing was changed.", res);
    }
    gui_message(msg);
}

/* Ask for the archive's capacity, which is fixed for its life — hence the
 * generous option, for a backup with a deep tree. 0 if the user backed out. */
int askCapacity(int* maxDirs, int* maxFiles)
{
    char* caps[3];
    int pick;

    caps[0] = "Standard - 100 folders, 1000 files";
    caps[1] = "Large - 1000 folders, 10000 files";
    caps[2] = "Enter my own limits";

    pick = gui_pick_one("Archive capacity (fixed once created)", caps, 3);
    if (pick < 0) {
        return 0;
    }

    if (pick == 0) {
        *maxDirs  = 100;
        *maxFiles = 1000;
        return 1;
    }
    if (pick == 1) {
        *maxDirs  = 1000;
        *maxFiles = CAP_MAX;
        return 1;
    }

    *maxDirs = gui_numpad("Maximum folders", 1, CAP_MAX);
    if (*maxDirs < 1) {
        return 0;
    }
    *maxFiles = gui_numpad("Maximum files", 1, CAP_MAX);
    if (*maxFiles < 1) {
        return 0;
    }
    return 1;
}

/* Pick a title with no extdata, then create its archive. */
void createFlow(void)
{
    int total;
    char** ids;
    char** names;
    char* sel;
    int n = 0;
    int i;
    int pick;
    char* id;
    char* name;
    int defaultId;
    int extdataId;
    int maxDirs;
    int maxFiles;
    int res;
    int which;
    char* choices[2];
    char useDefault[80];
    char line[MSGN];

    gui_status("Reading the title list...");
    total = titles_count();
    if (total == 0) {
        gui_message("Checkpoint's title list is empty.");
        return;
    }

    ids   = malloc(sizeof(char*) * total);
    names = malloc(sizeof(char*) * total);
    if (ids == NULL || names == NULL) {
        free(ids);
        free(names);
        gui_message("Not enough memory to read the title list.");
        return;
    }
    sel = selected_title();

    for (i = 0; i < total; i++) {
        char* tid = title_id(i);
        if (title_has_extdata(i)) {
            free(tid); /* it already has one; this script has nothing to add */
        }
        else {
            ids[n]   = tid;
            names[n] = title_name(i);
            /* offer the title highlighted in Checkpoint first */
            if (n > 0 && sel != NULL && strcmp(tid, sel) == 0) {
                char* t  = ids[n];
                ids[n]   = ids[0];
                ids[0]   = t;
                t        = names[n];
                names[n] = names[0];
                names[0] = t;
            }
            n = n + 1;
        }
    }
    if (sel != NULL) {
        free(sel);
    }

    sprintf(line, "%d of %d title(s) have no extdata archive", n, total);
    script_log(line);

    if (n == 0) {
        gui_message("Every title in the list already has an\nextdata archive.");
        free(ids);
        free(names);
        return;
    }

    pick = gui_pick_one("Create extdata for which title?", names, n);
    if (pick >= 0) {
        id        = ids[pick];
        name      = names[pick];
        defaultId = extdata_default_id(id);

        sprintf(line, "> %s [%s], default extdata id 0x%08X", name, id, defaultId);
        script_log(line);

        sprintf(useDefault, "Use 0x%08X (what Checkpoint looks for)", defaultId);
        choices[0] = useDefault;
        choices[1] = "Use a different id (advanced)";

        which = gui_pick_one("Extdata id", choices, 2);
        if (which < 0) {
            extdataId = -1;
        }
        else if (which == 0) {
            extdataId = defaultId;
        }
        else {
            extdataId = askExtdataId("Extdata id in hex");
        }

        if (extdataId >= 0 && askCapacity(&maxDirs, &maxFiles)) {
            sprintf(line, "Create an empty extdata archive for\n%s\nunder id 0x%08X?", name, extdataId);
            if (gui_confirm(line)) {
                gui_status("Creating the archive...");
                res = extdata_create(id, extdataId, maxDirs, maxFiles);
                sprintf(line, "extdata_create(%s, 0x%08X, %d, %d) = %d", id, extdataId, maxDirs, maxFiles, res);
                script_log(line);
                reportCreateResult(res, name, extdataId, defaultId);
            }
        }
    }

    for (i = 0; i < n; i++) {
        free(ids[i]);
        free(names[i]);
    }
    free(ids);
    free(names);
}

/* Read the root of any extdata id, listed or not. */
void inspectFlow(void)
{
    int extdataId;
    int count;
    char msg[192];

    extdataId = askExtdataId("Extdata id in hex");
    if (extdataId < 0) {
        return;
    }

    gui_status("Opening the archive...");
    count = describeExtdata(extdataId);
    if (count < 0) {
        sprintf(msg, "No extdata archive exists under\nid 0x%08X.", extdataId);
    }
    else {
        sprintf(msg, "extdata 0x%08X holds %d entr(y/ies)\nin its root. The names are in the log\nbelow.", extdataId, count);
    }
    gui_message(msg);
}

/* The undo for a create under the wrong id — and, pointed anywhere else, a
 * permanent delete of a save. Hence the two confirmations. */
void deleteFlow(void)
{
    int extdataId;
    int count;
    int res;
    char msg[MSGN];

    extdataId = askExtdataId("Extdata id to DELETE, in hex");
    if (extdataId < 0) {
        return;
    }

    gui_status("Opening the archive...");
    count = describeExtdata(extdataId);
    if (count < 0) {
        sprintf(msg, "No extdata archive exists under\nid 0x%08X. Nothing to delete.", extdataId);
        gui_message(msg);
        return;
    }

    sprintf(msg,
        "Permanently delete extdata 0x%08X\nand the %d entr(y/ies) in it?\n\nThis cannot be undone. If this archive\nholds save data, back it up first.",
        extdataId, count);
    if (!gui_confirm(msg)) {
        return;
    }

    sprintf(msg, "Last check: destroy extdata 0x%08X?", extdataId);
    if (!gui_confirm(msg)) {
        return;
    }

    gui_status("Deleting the archive...");
    res = extdata_delete(extdataId);
    sprintf(msg, "extdata_delete(0x%08X) = %d", extdataId, res);
    script_log(msg);

    if (res == 0) {
        sprintf(msg, "extdata 0x%08X deleted.\nHold B on the title list to refresh.", extdataId);
    }
    else if (res == -2) {
        sprintf(msg, "extdata 0x%08X was already gone.", extdataId);
    }
    else {
        sprintf(msg, "The console refused to delete it\n(result %d). Nothing was changed.", res);
    }
    gui_message(msg);
}

int main(int argc, char** argv)
{
    /* The bindings answer -1 where there is no extdata at all. */
    if (extdata_default_id("0004000000000000") < 0) {
        gui_message("This console has no extdata archives.");
        return 0;
    }

    while (1) {
        char* menu[4];
        int choice;

        menu[0] = "Create extdata for a title";
        menu[1] = "Inspect an extdata id";
        menu[2] = "Delete an extdata archive";
        menu[3] = "Exit";

        choice = gui_pick_one("Extdata", menu, 4);
        if (choice < 0 || choice == 3) {
            return 0;
        }
        if (choice == 0) {
            createFlow();
        }
        else if (choice == 1) {
            inspectFlow();
        }
        else {
            deleteFlow();
        }
    }
    return 0;
}
