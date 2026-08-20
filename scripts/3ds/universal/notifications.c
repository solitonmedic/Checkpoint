/*
 * notifications.c — back up and restore the console's SpotPass notification data.
 *
 * The Notifications applet owns no archive of its own, so it never appears in
 * Checkpoint's title list and there is nothing to select before running this.
 * What the applet shows is assembled from two places, neither of them a title:
 *
 *   - the SpotPass content the console downloaded, held in NAND *shared*
 *     extdata: 0xF0000009 (notification content), 0xF000000D (Home Menu
 *     SpotPass content) and 0xF000000E (the software update notice). Those are
 *     console-wide archives rather than per-title ones, which is why
 *     sav_open_shared reaches them and the Backup tab does not.
 *
 *   - the notification list itself — news.db, plus the newsNNN.txt and .mpo
 *     message files — which lives in the NEWS system module's savedata,
 *     0x00010035, reached with sav_open_system.
 *
 * All four are copied onto the SD card and written back. The NEWS savedata is
 * journalled rather than a plain file archive, so every write to it is
 * committed as it is made, and it is restored last: an abort part way through
 * a restore leaves the list as it was rather than half rewritten.
 *
 * Backups land in <app root>/notifications/<date>-<time>/, one folder per
 * archive, and are plain files a PC can read like any other backup.
 *
 * These archives belong to the console that wrote them, and a restore never
 * deletes what it does not overwrite: it is a way back from a wiped or a
 * half-eaten archive on the same console, not a way to move notifications
 * between two.
 */

#include <checkpoint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ARCHN       4        /* archives this script knows about              */
#define KIND_SHARED 0        /* shared extdata, opened by a 16-hex id         */
#define KIND_SYSTEM 1        /* NAND system savedata, opened by a 32-bit id   */
#define PATHN       512      /* a full path                                   */
#define MSGN        512      /* a dialog string                               */
#define NAMEN       128      /* one path component                            */
#define STAMPN      32       /* "20260820-153000"                             */
#define MAXDEPTH    8        /* recursion cap for the tree walkers            */
#define COPY_MAX    16777216 /* biggest file a copy may hold in RAM           */
#define MAXBACKUP   64       /* backup folders offered by the restore picker  */
#define LIST_MAX    12       /* entries printed when inspecting an archive    */

/* The archives. A shared one is keyed the way sav_open_shared wants it — the
 * low 32 bits the extdata id, the high 32 the shared-extdata magic — and a
 * system one by its plain savedata id. The slug is the folder a backup keeps
 * that archive in. */
int g_kind[ARCHN];
char* g_id[ARCHN];
int g_sys[ARCHN];
char* g_name[ARCHN];
char* g_slug[ARCHN];

/* What the recursive walkers report through. */
int g_copied;
int g_failed;

/* The rows handed to gui_pick_one, and the paths behind them: global because
 * both outlive the listing they were built from. */
char* g_rows[MAXBACKUP];
char* g_paths[MAXBACKUP];

/* app_root() hands back a block to free, so it is read once into here rather
 * than called from every path that builds a path. */
char g_root[PATHN];

void setupArchives(void)
{
    g_kind[0] = KIND_SHARED;
    g_id[0]   = "00048000F0000009";
    g_sys[0]  = 0;
    g_name[0] = "Notification content";
    g_slug[0] = "F0000009";

    g_kind[1] = KIND_SHARED;
    g_id[1]   = "00048000F000000D";
    g_sys[1]  = 0;
    g_name[1] = "Home Menu SpotPass content";
    g_slug[1] = "F000000D";

    g_kind[2] = KIND_SHARED;
    g_id[2]   = "00048000F000000E";
    g_sys[2]  = 0;
    g_name[2] = "Update notice list";
    g_slug[2] = "F000000E";

    /* Last on purpose: the notification list is the archive worth protecting,
     * so a restore that is aborted part way through has not touched it yet. */
    g_kind[3] = KIND_SYSTEM;
    g_id[3]   = "";
    g_sys[3]  = 0x00010035;
    g_name[3] = "Notification list (NEWS)";
    g_slug[3] = "00010035";
}

/* A handle on archive `i`, whichever kind it is. Negative when the console has
 * nothing under that id. */
int openArchive(int i)
{
    if (g_kind[i] == KIND_SYSTEM) {
        return sav_open_system(g_sys[i]);
    }
    return sav_open_shared(g_id[i]);
}

/* base + name, without ever producing "//": the FS rejects an empty component. */
void pathJoin(char* base, char* name, char* out, int size)
{
    int len = strlen(base);

    if (len > 0 && base[len - 1] == '/') {
        snprintf(out, size, "%s%s", base, name);
    }
    else {
        snprintf(out, size, "%s/%s", base, name);
    }
}

/* The last component of `full`, tolerating the trailing '/' sav_list puts on a
 * folder. */
void baseName(char* full, char* dst, int size)
{
    int len = strlen(full);
    int end;
    int start;
    int n;
    int i;

    end = len;
    if (end > 1 && full[end - 1] == '/') {
        end = end - 1;
    }

    start = end;
    while (start > 0 && full[start - 1] != '/') {
        start = start - 1;
    }

    n = end - start;
    if (n > size - 1) {
        n = size - 1;
    }
    for (i = 0; i < n; i++) {
        dst[i] = full[start + i];
    }
    dst[n] = '\0';
}

/* `path` without the trailing '/' a listing marks a folder with, so the result
 * is what sav_read and sav_list want. */
void stripSlash(char* path, char* out, int size)
{
    int len = strlen(path);
    int i;

    if (len > 1 && path[len - 1] == '/') {
        len = len - 1;
    }
    if (len > size - 1) {
        len = size - 1;
    }
    for (i = 0; i < len; i++) {
        out[i] = path[i];
    }
    out[len] = '\0';
}

/* read_directory says nothing about which entries are folders, so this is the
 * documented test: an entry that will not open, or will not read, is one. Only
 * for paths that came out of a listing, which therefore exist. */
int sdIsListedDir(char* path)
{
    char probe[1];
    FILE* f;
    int isdir;

    f = fopen(path, "rb");
    if (f == NULL) {
        return 1;
    }

    fread(probe, 1, 1, f);
    isdir = ferror(f) != 0;
    fclose(f);
    return isdir;
}

/* One file out of the archive onto the card. 0 ok; -2 the card refused the
 * file, -4 a short write, else the archive's own negative result. */
int pullFile(int h, char* apath, char* sdpath)
{
    char* data;
    int size;
    int res;
    int written;
    FILE* out;

    res = sav_read(h, apath, &data, &size);
    if (res != 0) {
        return res;
    }

    out = fopen(sdpath, "wb");
    if (out == NULL) {
        free(data);
        return -2;
    }

    written = size > 0 ? fwrite(data, 1, size, out) : 0;
    fclose(out);
    free(data);
    return written == size ? 0 : -4;
}

/* One file off the card into the archive, committed straight away so an abort
 * between two files cannot leave the last write half-done. 0 ok; -1 the card
 * file could not be read, -3 memory, -5 too big to pass through RAM, else the
 * archive's own negative result. */
int pushFile(int h, char* sdpath, char* apath)
{
    FILE* in;
    char* data;
    int size;
    int got;
    int res;

    in = fopen(sdpath, "rb");
    if (in == NULL) {
        return -1;
    }
    fseek(in, 0, SEEK_END);
    size = ftell(in);
    fseek(in, 0, SEEK_SET);

    if (size > COPY_MAX) {
        fclose(in);
        return -5;
    }

    data = (char*)malloc(size > 0 ? size : 1);
    if (data == NULL) {
        fclose(in);
        return -3;
    }

    got = size > 0 ? fread(data, 1, size, in) : 0;
    fclose(in);
    if (got != size) {
        free(data);
        return -1;
    }

    res = sav_write(h, apath, data, size);
    free(data);
    if (res == 0) {
        res = sav_commit(h);
    }
    return res;
}

/* Archive -> card, folders and all. */
void pullTree(int h, char* apath, char* sdDir, int depth)
{
    struct directory* d;
    char name[NAMEN];
    char child[PATHN];
    char dest[PATHN];
    char line[MSGN];
    char* raw;
    int isdir;
    int len;
    int res;
    int i;

    if (depth > MAXDEPTH) {
        snprintf(line, MSGN, "stopped at %s: nested deeper than %d", apath, MAXDEPTH);
        script_log(line);
        return;
    }

    d = sav_list(h, apath);
    if (d == NULL) {
        return;
    }

    for (i = 0; i < d->count; i++) {
        raw   = d->files[i];
        len   = strlen(raw);
        isdir = len > 0 && raw[len - 1] == '/';
        baseName(raw, name, NAMEN);
        stripSlash(raw, child, PATHN);
        pathJoin(sdDir, name, dest, PATHN);

        if (isdir) {
            if (sd_mkdirs(dest) != 0) {
                g_failed = g_failed + 1;
                snprintf(line, MSGN, "could not create %s", dest);
                script_log(line);
            }
            else {
                pullTree(h, child, dest, depth + 1);
            }
        }
        else {
            res = pullFile(h, child, dest);
            if (res == 0) {
                g_copied = g_copied + 1;
                progress_set(1, g_copied);
                progress_note(name);
            }
            else {
                g_failed = g_failed + 1;
                snprintf(line, MSGN, "could not copy out %s (%d)", child, res);
                script_log(line);
            }
        }
    }

    delete_directory(d);
}

/* Card -> archive. Files already in the archive are replaced; anything the
 * backup does not carry is left alone. */
void pushTree(int h, char* sdDir, char* apath, int depth)
{
    struct directory* d;
    char name[NAMEN];
    char child[PATHN];
    char line[MSGN];
    char* raw;
    int res;
    int i;

    if (depth > MAXDEPTH) {
        snprintf(line, MSGN, "stopped at %s: nested deeper than %d", sdDir, MAXDEPTH);
        script_log(line);
        return;
    }

    d = read_directory(sdDir);
    if (d == NULL) {
        return;
    }

    for (i = 0; i < d->count; i++) {
        raw = d->files[i];
        baseName(raw, name, NAMEN);
        pathJoin(apath, name, child, PATHN);

        if (sdIsListedDir(raw)) {
            /* one level, and a folder that is already there answers negative:
             * either way the children below can be written */
            sav_mkdir(h, child);
            pushTree(h, raw, child, depth + 1);
        }
        else {
            res = pushFile(h, raw, child);
            if (res == 0) {
                g_copied = g_copied + 1;
                progress_set(1, g_copied);
                progress_note(name);
            }
            else {
                g_failed = g_failed + 1;
                snprintf(line, MSGN, "could not write %s (%d)", child, res);
                script_log(line);
            }
        }
    }

    delete_directory(d);
}

/* Report what is in one archive's root. Returns the number of root entries, -1
 * if the console has no archive under that id. */
int describeArchive(int i)
{
    struct directory* d;
    char line[MSGN];
    int count;
    int h;
    int k;

    h = openArchive(i);
    if (h < 0) {
        snprintf(line, MSGN, "%s (%s): not on this console (%d)", g_name[i], g_slug[i], h);
        script_log(line);
        return -1;
    }

    d = sav_list(h, "/");
    if (d == NULL) {
        sav_close(h);
        return 0;
    }

    count = d->count;
    snprintf(line, MSGN, "%s (%s): %d entr(y/ies) in the root", g_name[i], g_slug[i], count);
    script_log(line);
    for (k = 0; k < count && k < LIST_MAX; k++) {
        printf("  %s\n", d->files[k]);
    }
    if (count > LIST_MAX) {
        printf("  ... and %d more\n", count - LIST_MAX);
    }

    delete_directory(d);
    sav_close(h);
    return count;
}

/* Copy every archive that exists to a fresh, timestamped folder. */
void backupFlow(void)
{
    char stamp[STAMPN];
    char root[PATHN];
    char dest[PATHN];
    char line[MSGN];
    char msg[MSGN];
    struct tm* local;
    int now;
    int present;
    int h;
    int i;

    now   = time(NULL);
    local = localtime(&now);
    strftime(stamp, STAMPN, "%Y%m%d-%H%M%S", local);
    snprintf(root, PATHN, "%s/notifications/%s", g_root, stamp);

    g_copied = 0;
    g_failed = 0;
    present  = 0;

    gui_status("Backing up notification data...");
    progress_begin(0, "Archives", ARCHN);
    progress_begin(1, "Files copied", 0);

    for (i = 0; i < ARCHN; i++) {
        progress_label(0, g_name[i]);
        h = openArchive(i);
        if (h < 0) {
            snprintf(line, MSGN, "%s (%s) is not on this console (%d)", g_name[i], g_slug[i], h);
            script_log(line);
        }
        else {
            present = present + 1;
            snprintf(dest, PATHN, "%s/%s", root, g_slug[i]);
            if (sd_mkdirs(dest) != 0) {
                g_failed = g_failed + 1;
                snprintf(line, MSGN, "could not create %s", dest);
                script_log(line);
            }
            else {
                pullTree(h, "/", dest, 0);
            }
            sav_close(h);
        }
        progress_set(0, i + 1);
    }

    progress_clear();

    if (present == 0) {
        gui_message("None of the notification archives\nexist on this console.");
        return;
    }

    snprintf(line, MSGN, "backed up %d file(s) from %d archive(s) into %s", g_copied, present, root);
    script_log(line);

    if (g_failed > 0) {
        snprintf(msg, MSGN, "%d file(s) copied from %d archive(s)\ninto notifications/%s.\n\n%d could not be copied — the names\nare in the log below.", g_copied,
            present, stamp, g_failed);
    }
    else {
        snprintf(msg, MSGN, "%d file(s) copied from %d archive(s)\ninto notifications/%s.", g_copied, present, stamp);
    }
    gui_message(msg);
}

/* Fill g_rows/g_paths with the backup folders on the card, newest last (the
 * names sort that way on their own). Returns how many there are. */
int listBackups(char* base)
{
    struct directory* d;
    char name[NAMEN];
    int count;
    int i;

    count = 0;
    d     = read_directory(base);
    if (d == NULL) {
        return 0;
    }

    for (i = 0; i < d->count && count < MAXBACKUP; i++) {
        if (sdIsListedDir(d->files[i])) {
            baseName(d->files[i], name, NAMEN);
            g_rows[count]  = strdup(name);
            g_paths[count] = strdup(d->files[i]);
            count          = count + 1;
        }
    }

    delete_directory(d);
    return count;
}

void freeBackups(int count)
{
    int i;

    for (i = 0; i < count; i++) {
        free(g_rows[i]);
        free(g_paths[i]);
    }
}

/* Write one backup folder back into the archives it was taken from. */
void restoreFlow(void)
{
    char base[PATHN];
    char src[PATHN];
    char line[MSGN];
    char msg[MSGN];
    int count;
    int pick;
    int found;
    int written;
    int h;
    int i;

    snprintf(base, PATHN, "%s/notifications", g_root);
    if (!sd_exists(base)) {
        gui_message("There is no notification backup on\nthis card yet.");
        return;
    }

    gui_status("Reading the backup folder...");
    count = listBackups(base);
    if (count == 0) {
        gui_message("There is no notification backup on\nthis card yet.");
        return;
    }

    pick = gui_pick_one("Restore which backup?", g_rows, count);
    if (pick < 0) {
        freeBackups(count);
        return;
    }

    /* Which archives this backup actually carries: a folder per slug. */
    found = 0;
    for (i = 0; i < ARCHN; i++) {
        snprintf(src, PATHN, "%s/%s", g_paths[pick], g_slug[i]);
        if (sd_exists(src)) {
            found = found + 1;
        }
    }

    if (found == 0) {
        snprintf(msg, MSGN, "%s holds no archive folder this\nscript knows about. Nothing to restore.", g_rows[pick]);
        gui_message(msg);
        freeBackups(count);
        return;
    }

    snprintf(msg, MSGN,
        "Write %s back into %d archive(s)\non this console, the notification list\namong them?\n\nFiles of the same name are replaced,\nanything the backup does not carry is\nleft alone. This is meant for the console\nthe backup was taken on.",
        g_rows[pick], found);
    if (!gui_confirm(msg)) {
        freeBackups(count);
        return;
    }

    g_copied = 0;
    g_failed = 0;
    written  = 0;

    gui_status("Restoring notification data...");
    progress_begin(0, "Archives", found);
    progress_begin(1, "Files written", 0);

    for (i = 0; i < ARCHN; i++) {
        snprintf(src, PATHN, "%s/%s", g_paths[pick], g_slug[i]);
        if (!sd_exists(src)) {
            continue;
        }

        progress_label(0, g_name[i]);
        h = openArchive(i);
        if (h < 0) {
            snprintf(line, MSGN, "%s (%s) could not be opened (%d)", g_name[i], g_slug[i], h);
            script_log(line);
        }
        else {
            pushTree(h, src, "/", 0);
            sav_close(h);
            written = written + 1;
        }
        progress_set(0, written);
    }

    progress_clear();

    snprintf(line, MSGN, "restored %d file(s) into %d archive(s) from %s", g_copied, written, g_paths[pick]);
    script_log(line);

    if (written == 0) {
        gui_message("None of the archives in that backup\ncould be opened. Nothing was changed.");
    }
    else if (g_failed > 0) {
        snprintf(msg, MSGN, "%d file(s) written into %d archive(s).\n\n%d could not be written — the names\nare in the log below.", g_copied, written, g_failed);
        gui_message(msg);
    }
    else {
        snprintf(msg, MSGN, "%d file(s) written into %d archive(s).", g_copied, written);
        gui_message(msg);
    }

    freeBackups(count);
}

/* What each archive holds right now, before deciding to touch any of it. */
void inspectFlow(void)
{
    char msg[MSGN];
    char row[NAMEN];
    int count;
    int i;

    gui_status("Opening the archives...");
    msg[0] = '\0';

    for (i = 0; i < ARCHN; i++) {
        count = describeArchive(i);
        if (count < 0) {
            snprintf(row, NAMEN, "%s: not present\n", g_slug[i]);
        }
        else {
            snprintf(row, NAMEN, "%s: %d entr(y/ies)\n", g_slug[i], count);
        }
        strncat(msg, row, MSGN - strlen(msg) - 1);
    }

    strncat(msg, "\nThe names are in the log below.", MSGN - strlen(msg) - 1);
    gui_message(msg);
}

int main(int argc, char** argv)
{
    char* menu[4];
    char* root;
    int choice;

    setupArchives();

    /* No title is selected when this runs — argv[0] is "" — so the app root is
     * the only path this script starts from. */
    root = app_root();
    strncpy(g_root, root, PATHN - 1);
    g_root[PATHN - 1] = '\0';
    free(root);

    while (1) {
        menu[0] = "Back up notification data";
        menu[1] = "Restore a backup";
        menu[2] = "Inspect the archives";
        menu[3] = "Exit";

        choice = gui_pick_one("Notifications", menu, 4);
        if (choice < 0 || choice == 3) {
            return 0;
        }
        if (choice == 0) {
            backupFlow();
        }
        else if (choice == 1) {
            restoreFlow();
        }
        else {
            inspectFlow();
        }
    }
    return 0;
}
