#include "FileServer.h"

// Compiled out by -D PEGASUS_WIFI_FILES=0. See FileServer.h: this is the only
// way the cost actually goes away, because the cost is code in the image and a
// runtime switch cannot un-link code.
#if PEGASUS_WIFI_FILES

#include <Arduino.h>
#include <SD_MMC.h>
#include <WebServer.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../navigation/GpxTrack.h"
#include "../navigation/RideLog.h"
#include "../sensors/BLE_HR_Client.h"
#include "FilePath.h"

namespace {

constexpr uint16_t HTTP_PORT = 80;
const IPAddress AP_IP(192, 168, 4, 1);

// One station. The access point exists for one phone or one laptop, and a
// lower limit is one less way for a neighbour to occupy a slot.
constexpr int MAX_STATIONS = 1;

// Fixed, at the owner's request, so it can be typed from memory rather than
// read off the panel each session.
//
// It is "pegasus1" and not "pegasus" because WPA2 will not take seven
// characters: esp_wifi refuses a passphrase under eight outright, and
// WiFi.softAP() returns false rather than falling back to an open network.
// Eight is the floor, not a preference.
//
// This is weaker than the per-session random password it replaces, and the
// reason that one existed is worth keeping written down: the access point
// broadcasts the chip's MAC as its BSSID, so anything derived from the MAC is
// published alongside the network it protects. A fixed word is not derived
// from anything, so it is only as weak as it is short -- fine for a device
// that is switched on for a few minutes beside its owner, and not something to
// leave running.
constexpr char AP_PASSWORD[] = "pegasus1";

// Streamed in chunks rather than read whole. A road extract is 1.8MB and a
// single read of one already tripped the task watchdog once.
constexpr size_t STREAM_CHUNK = 2048;

WebServer s_server(HTTP_PORT);
bool s_running = false;
char s_ssid[32] = "";
char s_password[sizeof(AP_PASSWORD)] = "";
char s_url[32] = "";
// The last byte is never written and stays NUL: SetStatus passes
// sizeof - 1 to vsnprintf, so index 95 is out of its reach. That is load
// bearing, not tidiness. The writer task fills this while the LVGL task reads
// it unlocked, and a long message replacing a short one passes through an
// instant with the old terminator gone and the new one not yet placed. A
// reader caught there stops at the array's own edge instead of running off it.
char s_status[96] = "idle";
volatile uint32_t s_requests = 0;

// The upload in flight. WebServer delivers a POST body in slices through a
// separate handler, so the open file has to live between calls.
File s_upload;
bool s_upload_ok = false;
// Separate from s_upload_ok, which is false once a good upload finishes too.
// This is what decides whether the response is a redirect or an error.
bool s_upload_failed = false;
char s_upload_path[FILEPATH_MAX] = "";

void SetStatus(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_status, sizeof(s_status) - 1, fmt, args);
    va_end(args);
}

const char *DirLabel(const char *dir) {
    if (strcmp(dir, FILEPATH_DIR_RIDES) == 0) {
        return "Recorded rides";
    }
    if (strcmp(dir, FILEPATH_DIR_MAP) == 0) {
        return "Road maps";
    }
    return "Routes to follow";
}

// HTML-escapes into the response. File names come off the card and are shown
// back, so a name containing "<" must not become a tag.
void SendEscaped(const char *text) {
    String out;
    for (const char *p = text; *p != '\0'; p++) {
        switch (*p) {
            case '&':  out += F("&amp;");  break;
            case '<':  out += F("&lt;");   break;
            case '>':  out += F("&gt;");   break;
            case '"':  out += F("&quot;"); break;
            case '\'': out += F("&#39;");  break;
            default:   out += *p;          break;
        }
    }
    s_server.sendContent(out);
}

void SendListing(const char *dir) {
    s_server.sendContent(F("<h2>"));
    SendEscaped(DirLabel(dir));
    s_server.sendContent(F("</h2><p class=p>"));
    SendEscaped(dir);
    if (strcmp(dir, FILEPATH_DIR_ROOT) == 0) {
        s_server.sendContent(F(" &mdash; .gpx only"));
    }
    s_server.sendContent(F("</p><table>"));

    File folder = SD_MMC.open(dir);
    int shown = 0;
    if (folder && folder.isDirectory()) {
        for (File entry = folder.openNextFile(); entry; entry = folder.openNextFile()) {
            if (entry.isDirectory()) {
                continue;
            }
            // The same rule the download and delete handlers apply, so the
            // page never offers a link those would refuse.
            const char *leaf = strrchr(entry.name(), '/');
            leaf = (leaf != nullptr) ? leaf + 1 : entry.name();
            if (!FilePath_LeafAllowedIn(dir, leaf)) {
                continue;
            }

            char path[FILEPATH_MAX];
            if (!FilePath_Build(dir, leaf, path, sizeof(path))) {
                continue;
            }

            s_server.sendContent(F("<tr><td><a href=\"/dl?p="));
            SendEscaped(path);
            s_server.sendContent(F("\">"));
            SendEscaped(leaf);
            s_server.sendContent(F("</a></td><td class=n>"));
            s_server.sendContent(String((uint32_t)entry.size()));
            s_server.sendContent(F("</td><td><form method=POST action=\"/rm\" "
                                   "onsubmit=\"return confirm('Delete?')\">"
                                   "<input type=hidden name=p value=\""));
            SendEscaped(path);
            s_server.sendContent(F("\"><button class=d>delete</button></form></td></tr>"));
            shown++;
        }
    }
    if (shown == 0) {
        s_server.sendContent(F("<tr><td colspan=3 class=p>empty</td></tr>"));
    }
    s_server.sendContent(F("</table>"
                           "<form method=POST action=\"/up\" enctype=\"multipart/form-data\">"
                           "<input type=hidden name=d value=\""));
    SendEscaped(dir);
    s_server.sendContent(F("\"><input type=file name=f required>"
                           "<button>Upload here</button></form>"));
}

void HandleIndex() {
    s_requests++;
    s_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    s_server.send(200, "text/html", "");

    s_server.sendContent(F("<!doctype html><meta charset=utf-8>"
                           "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
                           "<title>Pegasus files</title><style>"
                           "body{font:16px system-ui,sans-serif;margin:0;padding:16px;"
                           "background:#0d1720;color:#e8eef4}"
                           "h1{font-size:20px;margin:0 0 4px}h2{font-size:16px;margin:24px 0 2px}"
                           ".p{color:#93a4b8;font-size:13px;margin:0 0 8px}"
                           "table{width:100%;border-collapse:collapse;margin-bottom:8px}"
                           "td{padding:8px 4px;border-bottom:1px solid #1d2b38;"
                           "word-break:break-all}"
                           "td.n{text-align:right;color:#93a4b8;white-space:nowrap;"
                           "font-variant-numeric:tabular-nums}"
                           "a{color:#61dafb}form{display:inline}"
                           "button{background:#1d2b38;color:#e8eef4;border:0;border-radius:6px;"
                           "padding:8px 12px;font-size:14px}"
                           "button.d{background:none;color:#ff6b6b;padding:4px}"
                           "input[type=file]{max-width:60%;font-size:13px}"
                           "</style><h1>Pegasus files</h1>"
                           "<p class=p>Sizes in bytes.</p>"));

    SendListing(FILEPATH_DIR_RIDES);
    SendListing(FILEPATH_DIR_ROOT);
    SendListing(FILEPATH_DIR_MAP);

    s_server.sendContent("");
    SetStatus("listed");
}

void HandleDownload() {
    s_requests++;
    const String path = s_server.arg("p");
    if (!FilePath_Allowed(path.c_str())) {
        SetStatus("refused a download: %s", path.c_str());
        s_server.send(403, "text/plain", "not allowed");
        return;
    }

    File f = SD_MMC.open(path.c_str(), FILE_READ);
    if (!f || f.isDirectory()) {
        SetStatus("not found: %s", path.c_str());
        s_server.send(404, "text/plain", "no such file");
        return;
    }

    const char *leaf = strrchr(path.c_str(), '/') + 1;
    s_server.sendHeader("Content-Disposition", String("attachment; filename=\"") + leaf + "\"");
    // streamFile reads in chunks of its own, so a 1.8MB map never becomes a
    // single allocation or a single blocking read.
    s_server.streamFile(f, "application/octet-stream");
    f.close();
    SetStatus("sent %s", leaf);
}

void HandleDelete() {
    s_requests++;
    const String path = s_server.arg("p");
    if (!FilePath_Allowed(path.c_str())) {
        SetStatus("refused a delete: %s", path.c_str());
        s_server.send(403, "text/plain", "not allowed");
        return;
    }
    const bool ok = SD_MMC.remove(path.c_str());
    SetStatus(ok ? "deleted %s" : "could not delete %s", path.c_str());
    s_server.sendHeader("Location", "/");
    s_server.send(303, "text/plain", "");
}

// Called repeatedly by WebServer as the body arrives, then once more at the
// end. The path is decided on the first slice and never revisited, so a
// crafted second part cannot redirect a write that is already open.
void HandleUploadBody() {
    HTTPUpload &upload = s_server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        s_upload_ok = false;
        s_upload_failed = true; // until it isn't
        s_upload_path[0] = '\0';

        // The target directory is a hidden field placed BEFORE the file input
        // in the form, and that order is load-bearing: multipart parts arrive
        // in document order, so a field after the file would not be parsed yet
        // when this runs and every upload would land nowhere.
        const String dir = s_server.arg("d");
        // The browser may send a full path as the name on some platforms;
        // take the leaf and let FilePath judge it.
        const char *name = upload.filename.c_str();
        const char *slash = strrchr(name, '/');
        const char *backslash = strrchr(name, '\\');
        if (slash != nullptr) {
            name = slash + 1;
        }
        if (backslash != nullptr && backslash + 1 > name) {
            name = backslash + 1;
        }

        if (!FilePath_Build(dir.c_str(), name, s_upload_path, sizeof(s_upload_path))) {
            SetStatus("refused an upload: %s", upload.filename.c_str());
            return;
        }

        s_upload = SD_MMC.open(s_upload_path, FILE_WRITE);
        if (!s_upload) {
            SetStatus("could not create %s", s_upload_path);
            s_upload_path[0] = '\0';
            return;
        }
        s_upload_ok = true;
        SetStatus("receiving %s", s_upload_path);
        return;
    }

    if (!s_upload_ok) {
        return;
    }

    if (upload.status == UPLOAD_FILE_WRITE) {
        if (s_upload.write(upload.buf, upload.currentSize) != upload.currentSize) {
            // Out of space, or the card went away. Close and remove the
            // fragment rather than leave a half file that looks complete.
            s_upload.close();
            SD_MMC.remove(s_upload_path);
            s_upload_ok = false;
            SetStatus("write failed, removed %s", s_upload_path);
        }
        return;
    }

    if (upload.status == UPLOAD_FILE_END) {
        s_upload.close();
        s_upload_ok = false;
        s_upload_failed = false;
        SetStatus("received %s (%u bytes)", s_upload_path, (unsigned)upload.totalSize);
        return;
    }

    // UPLOAD_FILE_ABORTED: the client vanished mid-body.
    s_upload.close();
    SD_MMC.remove(s_upload_path);
    s_upload_ok = false;
    SetStatus("upload aborted, removed %s", s_upload_path);
}

void HandleUploadDone() {
    s_requests++;
    // Say why rather than redirect to a listing that looks unchanged for no
    // stated reason -- which is what a refused extension or a full card would
    // otherwise look like from the phone.
    if (s_upload_failed) {
        s_server.send(400, "text/plain", s_status);
        return;
    }
    s_server.sendHeader("Location", "/");
    s_server.send(303, "text/plain", "");
}

void HandleNotFound() {
    s_requests++;
    s_server.send(404, "text/plain", "no such page");
}

// Its own task on Core 0, beside the other background work. Serving from
// loop() would put multi-megabyte card reads on the LVGL thread, which is the
// shape of the watchdog reset this project already had once.
void ServerTask(void *pv) {
    (void)pv;
    for (;;) {
        s_server.handleClient();
        // handleClient() returns immediately when nothing is pending, so
        // without this the task spins at full priority on an idle link.
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

} // namespace

bool FileServer_Start() {
    if (s_running) {
        return true;
    }

    if (!GpxTrack_CardMounted()) {
        SetStatus("no SD card was mounted at boot");
        return false;
    }
    // The HTTP task and the ride-log writer would otherwise both be inside
    // SD_MMC at once, and the driver does not promise that is safe.
    if (RideLog_IsRecording()) {
        SetStatus("a ride is being recorded -- not while it is");
        return false;
    }

    // Take the radio outright rather than trust coexistence. See the header.
    BLE_HR_Shutdown();

    uint8_t mac[6] = {0};
    WiFi.macAddress(mac);
    snprintf(s_ssid, sizeof(s_ssid), "Pegasus-%02X%02X", mac[4], mac[5]);
    snprintf(s_password, sizeof(s_password), "%s", AP_PASSWORD);

    WiFi.mode(WIFI_AP);
    if (!WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0))) {
        SetStatus("could not configure the access point");
        return false;
    }
    // Channel 1, not hidden, one station. WPA2 comes from passing a password
    // at all -- an open access point here would be an open file server.
    if (!WiFi.softAP(s_ssid, s_password, 1, 0, MAX_STATIONS)) {
        SetStatus("could not start the access point");
        WiFi.mode(WIFI_OFF);
        return false;
    }

    snprintf(s_url, sizeof(s_url), "http://%s", WiFi.softAPIP().toString().c_str());

    s_server.on("/", HTTP_GET, HandleIndex);
    s_server.on("/dl", HTTP_GET, HandleDownload);
    s_server.on("/rm", HTTP_POST, HandleDelete);
    s_server.on("/up", HTTP_POST, HandleUploadDone, HandleUploadBody);
    s_server.onNotFound(HandleNotFound);
    s_server.begin();

    xTaskCreatePinnedToCore(ServerTask, "fileserver", 8192, nullptr, 1, nullptr, 0);

    s_running = true;
    s_requests = 0;
    SetStatus("waiting for a device to join");
    return true;
}

void FileServer_StopAndRestart() {
    s_server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    // Long enough for the card to settle and the station to see the access
    // point go, short enough not to look like a hang.
    delay(200);
    ESP.restart();
}

bool FileServer_IsRunning() {
    return s_running;
}

const char *FileServer_Ssid() {
    return s_ssid;
}

const char *FileServer_Password() {
    return s_password;
}

const char *FileServer_Url() {
    return s_url;
}

int FileServer_ClientCount() {
    return s_running ? WiFi.softAPgetStationNum() : 0;
}

uint32_t FileServer_RequestCount() {
    return s_requests;
}

const char *FileServer_StatusText() {
    return s_status;
}

#else // PEGASUS_WIFI_FILES

// The same API, answering honestly. Keeping the surface identical means the
// settings page and the overlay compile either way and the flag is one line
// in platformio.ini rather than ifdefs scattered through the UI.
bool FileServer_Start() { return false; }
void FileServer_StopAndRestart() {}
bool FileServer_IsRunning() { return false; }
const char *FileServer_Ssid() { return ""; }
const char *FileServer_Password() { return ""; }
const char *FileServer_Url() { return ""; }
int FileServer_ClientCount() { return 0; }
uint32_t FileServer_RequestCount() { return 0; }
const char *FileServer_StatusText() { return "not built into this firmware"; }

#endif // PEGASUS_WIFI_FILES
