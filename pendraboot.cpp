// Pendraboot — Formatea y crea USB booteables en FLinux (32 bits)
// C++ / FLTK 1.3. Se ejecuta setuid root (formatear y dd exigen root).
// Uso: pendraboot
// Seguridad dura: solo lista dispositivos sd* con removable==1; nunca el
// disco interno ni el dispositivo de arranque (cde/tcedir).
// Antes de grabar, la ISO se hace híbrida con isohybrid (syslinux) salvo
// que ya lo sea; en la vía local se trabaja sobre una copia temporal y
// el original del usuario nunca se modifica.

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Round_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Pixmap.H>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>

// ---------------- URLs oficiales de FLinux (SourceForge, proyecto flinux-distro) ----------------
// Verificación: la página del proyecto resuelve y la URL de descarga
// responde HTTP 200 (cuerpo binario). La descarga se valida en la app
// (curl -f + tamaño > 1 MB) antes de cualquier dd.
static const char* URL_WIFI    = "https://sourceforge.net/projects/flinux-distro/files/flinux-17.2+wifi.iso/download";
static const char* URL_NOWIFI  = "https://sourceforge.net/projects/flinux-distro/files/flinux-17.2.iso/download";

// ---------------- utilidades ----------------
static std::string trim(const std::string& s) {
    size_t a = 0; while (a < s.size() && isspace((unsigned char)s[a])) a++;
    size_t b = s.size(); while (b > a && isspace((unsigned char)s[b-1])) b--;
    return s.substr(a, b - a);
}
static std::string read_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return "";
    char buf[1024]; std::string out;
    size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}
static std::string human_size(unsigned long long bytes) {
    char buf[64];
    if (bytes >= 1000000000ULL) snprintf(buf, sizeof buf, "%.1f GB", bytes / 1e9);
    else if (bytes >= 1000000ULL) snprintf(buf, sizeof buf, "%.0f MB", bytes / 1e6);
    else if (bytes >= 1000ULL) snprintf(buf, sizeof buf, "%.0f KB", bytes / 1e3);
    else snprintf(buf, sizeof buf, "%llu B", bytes);
    return buf;
}
static std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
    out += "'";
    return out;
}

// ---------------- detección USB ----------------
struct UsbDev {
    std::string name;    // sdb
    std::string model;
    std::string size;    // "32 GB"
    unsigned long long bytes;
    std::string format;  // FAT32, ext4, sin formato...
};

// Dispositivos de arranque/sistema a excluir, resueltos desde /proc/mounts
// a partir de /etc/sysconfig/cde y /etc/sysconfig/tcedir.
static std::vector<std::string> partitions_of(const std::string& dev);
static std::vector<std::string> system_devices() {
    std::vector<std::string> out;
    std::string mounts = read_file("/proc/mounts");
    std::vector<std::string> roots;
    std::string cde = trim(read_file("/etc/sysconfig/cde"));
    std::string tce = trim(read_file("/etc/sysconfig/tcedir"));
    if (!cde.empty()) roots.push_back(cde);
    if (!tce.empty()) roots.push_back(tce);
    // /mnt/sr0 es el CD de arranque en Tiny Core
    roots.push_back("/mnt/sr0");
    // Para cada punto de montaje del sistema, busca su dispositivo
    size_t pos = 0;
    while (pos < mounts.size()) {
        size_t eol = mounts.find('\n', pos);
        std::string line = mounts.substr(pos, eol == std::string::npos ? eol : eol - pos);
        // formato: dispositivo punto_montaje resto
        size_t sp1 = line.find(' ');
        size_t sp2 = sp1 == std::string::npos ? sp1 : line.find(' ', sp1 + 1);
        if (sp1 != std::string::npos && sp2 != std::string::npos) {
            std::string dev = line.substr(0, sp1);
            std::string mp = line.substr(sp1 + 1, sp2 - sp1 - 1);
            for (size_t i = 0; i < roots.size(); i++) {
                // Coincide el punto de montaje exacto o un subdirectorio
                // suyo (tcedir es, p. ej., /mnt/sdb1/tce bajo /mnt/sdb1).
                if (mp == roots[i] || roots[i].rfind(mp + "/", 0) == 0) {
                    // dev puede ser /dev/sdb1 -> dispositivo base sdb
                    if (dev.rfind("/dev/", 0) == 0) {
                        std::string base = dev.substr(5);
                        while (!base.empty() && isdigit((unsigned char)base.back())) base.pop_back();
                        if (base.rfind("sd", 0) == 0) out.push_back(base);
                    }
                }
            }
        }
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return out;
}

static std::vector<UsbDev> scan_usb() {
    std::vector<UsbDev> out;
    std::vector<std::string> sysdevs = system_devices();
    DIR* d = opendir("/sys/block");
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        std::string name = e->d_name;
        // Solo sd*: el almacenamiento USB siempre aparece como sdX.
        if (name.size() < 3 || name.compare(0, 2, "sd") != 0) continue;
        bool allalpha = true;
        for (size_t i = 2; i < name.size(); i++) if (!isalpha((unsigned char)name[i])) allalpha = false;
        if (!allalpha) continue;  // solo el dispositivo, no sus particiones
        std::string rem = trim(read_file("/sys/block/" + name + "/removable"));
        if (rem != "1") continue;  // disco interno: nunca
        bool excluded = false;
        for (size_t i = 0; i < sysdevs.size(); i++) if (sysdevs[i] == name) excluded = true;
        if (excluded) continue;    // dispositivo de arranque: nunca
        UsbDev dev;
        dev.name = name;
        dev.model = trim(read_file("/sys/block/" + name + "/device/model"));
        if (dev.model.empty()) dev.model = "USB";
        std::string sz = trim(read_file("/sys/block/" + name + "/size"));
        unsigned long long sectors = sz.empty() ? 0 : strtoull(sz.c_str(), NULL, 10);
        dev.bytes = sectors * 512ULL;
        dev.size = human_size(dev.bytes);
        // Formato actual: primera partición, o el dispositivo entero.
        // blkid (busybox o util-linux) imprime: /dev/sdb1: UUID="..." TYPE="vfat"
        dev.format = "sin formato";
        std::vector<std::string> parts = partitions_of(name);
        std::string target = parts.empty() ? ("/dev/" + name) : parts[0];
        std::string cmd = "blkid " + target + " 2>/dev/null";
        FILE* p = popen(cmd.c_str(), "r");
        if (p) {
            char buf[512];
            if (fgets(buf, sizeof buf, p)) {
                std::string line = buf;
                size_t t = line.find("TYPE=\"");
                if (t != std::string::npos) {
                    size_t s = t + 6;
                    size_t e = line.find('"', s);
                    if (e != std::string::npos && e > s) dev.format = line.substr(s, e - s);
                }
            }
            pclose(p);
        }
        out.push_back(dev);
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const UsbDev& a, const UsbDev& b){ return a.name < b.name; });
    return out;
}

static std::vector<std::string> partitions_of(const std::string& dev) {
    std::vector<std::string> out;
    DIR* pd = opendir(("/sys/block/" + dev).c_str());
    if (!pd) return out;
    struct dirent* pe;
    while ((pe = readdir(pd)) != NULL) {
        std::string pn = pe->d_name;
        if (pn.size() > dev.size() && pn.compare(0, dev.size(), dev) == 0)
            out.push_back("/dev/" + pn);
    }
    closedir(pd);
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------- ejecutor asíncrono ----------------
// Lanza un comando en un hijo con su salida a un tubo; el padre lee por
// temporizador y vuelca la salida en el registro sin congelar la interfaz.
class AsyncRunner {
public:
    pid_t pid = -1;
    int fd = -1;
    std::string buf;
    bool running = false;
    // progreso opcional: fichero cuyo tamaño se compara con total
    std::string progress_file;
    unsigned long long progress_total = 0;
    // progreso dd: total de la ISO para parsear "N bytes"
    unsigned long long dd_total = 0;

    bool start(const std::string& cmd) {
        int pipefd[2];
        if (pipe(pipefd) != 0) return false;
        pid = fork();
        if (pid == 0) {
            dup2(pipefd[1], 1);
            dup2(pipefd[1], 2);
            close(pipefd[0]);
            close(pipefd[1]);
            execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)NULL);
            _exit(127);
        }
        close(pipefd[1]);
        fd = pipefd[0];
        fcntl(fd, F_SETFL, O_NONBLOCK);
        buf.clear();
        running = true;
        return true;
    }
    // Lee lo disponible; devuelve líneas nuevas. -1 si terminó.
    int poll(std::string& out_lines) {
        out_lines.clear();
        if (!running) return -1;
        char tmp[4096];
        ssize_t n;
        while ((n = read(fd, tmp, sizeof tmp)) > 0) {
            buf.append(tmp, n);
            out_lines.append(tmp, n);
        }
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            // drenar resto
            while ((n = read(fd, tmp, sizeof tmp)) > 0) {
                buf.append(tmp, n);
                out_lines.append(tmp, n);
            }
            close(fd);
            fd = -1;
            running = false;
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            return 1;
        }
        return -2; // sigue corriendo
    }
    void kill() {
        if (running && pid > 0) { ::kill(pid, 9); }
        if (fd >= 0) close(fd);
        running = false;
    }
};

// Iconos XPM embebidos (generados desde pendraboot.png y los
// PNG sol/luna con gen_icons_xpm.py; Fl_Pixmap vive en libfltk
// y no necesita libfltk_images ni RTTI, a diferencia de
// Fl_PNG_Image, que no enlaza en el FLTK sin RTTI de Tiny Core).
static const char* icon36_xpm[] = {
"36 36 413 2",
"aa c None",
"ab c #ffff00",
"ac c #ffff00",
"ad c #ffff55",
"ae c #ffffff",
"af c #ff7f7f",
"ag c #c4a658",
"ah c #f7d15f",
"ai c #f7d970",
"aj c #d6c9ae",
"ak c #ffff7f",
"al c #bfbf3f",
"am c #ceac52",
"an c #f4ce54",
"ao c #ffdc56",
"ap c #ffda57",
"aq c #fad966",
"ar c #f4d982",
"as c None",
"at c #ffffbf",
"au c #d8b24c",
"av c #f8d152",
"aw c #fed554",
"ax c #f2ca4e",
"ay c #ffd350",
"az c #fbd253",
"aA c #ffdf61",
"aB c #f9da70",
"aC c #eecd69",
"aD c #ffffff",
"aE c #ffaa55",
"aF c #d1b047",
"aG c #f2ce51",
"aH c #f4d252",
"aI c #867237",
"aJ c #766633",
"aK c #ccac47",
"aL c #f6ce52",
"aM c #f3ca50",
"aN c #ffd850",
"aO c #f5df9f",
"aP c #efedeb",
"aQ c #c4c4c4",
"aR c #7f7f7f",
"aS c #d2ac4b",
"aT c #f1c950",
"aU c #ffdf54",
"aV c #ab8f3c",
"aW c #5c512f",
"aX c #675a33",
"aY c #9a833c",
"aZ c #fed555",
"a0 c #f2c948",
"a1 c #ebd18a",
"a2 c #e7e4dd",
"a3 c #f8f4e7",
"a4 c #efeae0",
"a5 c #ebe8e2",
"a6 c #bfbfbf",
"a7 c #c9aa35",
"a8 c #f1c94b",
"a9 c #fed450",
"a! c #f0c74b",
"a# c #f9cf4e",
"a$ c #b39841",
"a% c #a1883c",
"a& c #ffd755",
"a* c #f4c947",
"a+ c #eed387",
"a- c #e5e2dd",
"a; c #ece8db",
"a< c #f8f3e6",
"a= c #fbf7ea",
"a> c #f2eee3",
"a? c #f2ece5",
"a@ c #e1e1d9",
"a^ c #aaaaaa",
"a_ c #dace6d",
"a| c #eec74f",
"a~ c #ffd651",
"ba c #f1c94a",
"bb c #f3ca4c",
"bc c #f2c94d",
"bd c #fed352",
"be c #fed552",
"bf c #f2c844",
"bg c #edd07e",
"bh c #e6e3dc",
"bi c #f2ede1",
"bj c #d8d4c8",
"bk c #6e6c66",
"bl c #a39f96",
"bm c #f8f3e6",
"bn c #f7f3e6",
"bo c #f0ebdf",
"bp c #eeeae0",
"bq c #00007f",
"br c #d0a124",
"bs c #ffcf3f",
"bt c #f3cc50",
"bu c #f3ca4b",
"bv c #f3c94c",
"bw c #f4cb4f",
"bx c #f2cb51",
"by c #f2c946",
"bz c #edcf78",
"bA c #e5e1d8",
"bB c #ede9dd",
"bC c #88857d",
"bD c #2c2c2a",
"bE c #494744",
"bF c #95928a",
"bG c #62605b",
"bH c #dedace",
"bI c #f2ece3",
"bJ c #eae5dd",
"bK c #b37e1a",
"bL c #c88c0b",
"bM c #e3b125",
"bN c #f4cb49",
"bO c #f4cc53",
"bP c #f2cb53",
"bQ c #f3c944",
"bR c #eece70",
"bS c #e6e1d6",
"bT c #e5e0d5",
"bU c #faf4e5",
"bV c #75736d",
"bW c #101010",
"bX c #87847d",
"bY c #797871",
"bZ c #585652",
"b0 c #908d86",
"b1 c #bab6ad",
"b2 c #ebe7db",
"b3 c #f1ece1",
"b4 c #f0ebe2",
"b5 c #cfdfcf",
"b6 c #ffffff",
"b7 c #a5791b",
"b8 c #b0790f",
"b9 c #af7609",
"b! c #d29b13",
"b# c #edc23b",
"b$ c #fbd250",
"b% c #f6d470",
"b& c #e5ded1",
"b* c #e6e1d7",
"b+ c #eee8db",
"b- c #cdc9be",
"b; c #c5c1b6",
"b< c #b7b3aa",
"b= c #2b2b29",
"b> c #c5c1b7",
"b? c #82807a",
"b@ c #d2cdc2",
"b^ c #fdf8eb",
"b_ c #e7e3d7",
"b| c #e6e2d6",
"b~ c #fbf6ea",
"ca c #f2ede3",
"cb c #edeae1",
"cc c #966b14",
"cd c #9f6f08",
"ce c #a4730a",
"cf c #a6710b",
"cg c #c58b0a",
"ch c #ddad26",
"ci c #e5ddcc",
"cj c #faf5ea",
"ck c #e7dfd1",
"cl c #f4efe2",
"cm c #71706a",
"cn c #716e69",
"co c #faf5e8",
"cp c #383735",
"cq c #98968e",
"cr c #4a4946",
"cs c #a09d95",
"ct c #676561",
"cu c #898780",
"cv c #e9e4d8",
"cw c #eee8dc",
"cx c #eae6da",
"cy c #f0ebdf",
"cz c #f9f4e8",
"cA c #f3eee4",
"cB c #e4e4dd",
"cC c #ffffaa",
"cD c #7d5603",
"cE c #916302",
"cF c #a27006",
"cG c #af7a0c",
"cH c #a06d08",
"cI c #ccffff",
"cJ c #e7decd",
"cK c #ece5d7",
"cL c #f4efe1",
"cM c #ddd7cc",
"cN c #f6efe3",
"cO c #8f8c85",
"cP c #565552",
"cQ c #777570",
"cR c #5a5854",
"cS c #807d78",
"cT c #444341",
"cU c #54524f",
"cV c #84817b",
"cW c #e5e0d4",
"cX c #d6d2c6",
"cY c #eeeadd",
"cZ c #f8f2e6",
"c0 c #f3efe4",
"c1 c #f2ede5",
"c2 c #aad4d4",
"c3 c #7f7f00",
"c4 c #443300",
"c5 c #875c01",
"c6 c #a57104",
"c7 c #95690a",
"c8 c #dcd2c8",
"c9 c #eae4d7",
"c! c #f9f1e3",
"c# c #ece4d7",
"c$ c #dad6cb",
"c% c #d7d3c8",
"c& c #5f5d59",
"c* c #282827",
"c+ c #e5e1d4",
"c- c #6b6964",
"c; c #74726d",
"c< c #484644",
"c= c #9e9b93",
"c> c #e6e2d6",
"c? c #dcd7cc",
"c@ c #f6f2e5",
"c^ c #ebe7db",
"c_ c #f2ede3",
"c| c #e8e8dd",
"c~ c #aa5500",
"da c #7f5703",
"db c #815904",
"dc c #d4aa7f",
"dd c #000000",
"de c #e4ddd2",
"df c #ece5d8",
"dg c #f7f1e5",
"dh c #e8e3d6",
"di c #f5f0e3",
"dj c #aaa79e",
"dk c #a8a59c",
"dl c #a29f97",
"dm c #363635",
"dn c #7d7b75",
"do c #51504d",
"dp c #f5f0e4",
"dq c #87847e",
"dr c #b7b4ab",
"ds c #e9e4d9",
"dt c #f6f1e5",
"du c #f5f1e4",
"dv c #f5f0e6",
"dw c #f1ece3",
"dx c #d0d0c5",
"dy c #000000",
"dz c #dbd6cc",
"dA c #e9e4d8",
"dB c #f5f0e3",
"dC c #e9e5d9",
"dD c #ece8dc",
"dE c #a19e96",
"dF c #b9b5ac",
"dG c #3f3e3c",
"dH c #96938c",
"dI c #928f89",
"dJ c #4b4a47",
"dK c #d0cdc2",
"dL c #41413f",
"dM c #d9d5ca",
"dN c #aba89f",
"dO c #e2ded2",
"dP c #eae5d9",
"dQ c #fcf5ea",
"dR c #f6f0e7",
"dS c #e8e4de",
"dT c #e2ddd1",
"dU c #ebe6da",
"dV c #f4efe2",
"dW c #e3dfd3",
"dX c #efeade",
"dY c #b2afa6",
"dZ c #1d1d1d",
"d0 c #bebab1",
"d1 c #a8a59d",
"d2 c #474644",
"d3 c #595855",
"d4 c #ded9ce",
"d5 c #ebe7db",
"d6 c #ece6db",
"d7 c #fffcf1",
"d8 c #e3dfd6",
"d9 c #d2cac3",
"d! c #e8e3d6",
"d# c #f5efe2",
"d$ c #ebe6d9",
"d% c #e3dfd3",
"d& c #f4eee1",
"d* c #e4dfd3",
"d+ c #c8c5ba",
"d- c #5e5d59",
"d; c #8c8983",
"d< c #fffdef",
"d= c #eae6da",
"d> c #ebe7db",
"d? c #e4dfd4",
"d@ c #b3acac",
"d^ c #c4bdb7",
"d_ c #e2ddd2",
"d| c #ede6da",
"d~ c #f9f4e7",
"ea c #e4dfd4",
"eb c #e9e3d7",
"ec c #f3eee2",
"ed c #d8d5ca",
"ee c #75726d",
"ef c #7c7a74",
"eg c #e7e3d7",
"eh c #fdf7eb",
"ei c #e6e0d7",
"ej c #d0cdc7",
"ek c #bdbab7",
"el c #cfc6bc",
"em c #e8e3d6",
"en c #f2ece0",
"eo c #eeeade",
"ep c #e4dfd4",
"eq c #eae6da",
"er c #dcd6cc",
"es c #d8d4c9",
"et c #ebe7db",
"eu c #fff8ec",
"ev c #e8e3d8",
"ew c #cfcbc5",
"ex c #c6c3bd",
"ey c #e1dbd1",
"ez c #ece6da",
"eA c #e7e3d7",
"eB c #e6e2d6",
"eC c #ebe6da",
"eD c #fcf7ea",
"eE c #eae4d9",
"eF c #e6e0d8",
"eG c #fbf7ef",
"eH c #d7d1cc",
"eI c #d2cbc6",
"eJ c #c5b9b9",
"eK c #c6b8b8",
"eL c #e9e2d7",
"eM c #ece7db",
"eN c #f7f0e4",
"eO c #e6e1d5",
"eP c #f8f3e6",
"eQ c #e9e4da",
"eR c #dad3cc",
"eS c #d3cdc8",
"eT c #f1eee6",
"eU c #e2dfd7",
"eV c #cdc9c4",
"eW c #f0ede6",
"eX c #e6e3dd",
"eY c #ffd4d4",
"eZ c #e0d6cc",
"e0 c #e9e4d7",
"e1 c #f9f5e9",
"e2 c #ebe5db",
"e3 c #d0d0b9",
"e4 c #e5ddd5",
"e5 c #eee6de",
"e6 c #e3ded5",
"e7 c #e0dbd3",
"e8 c #e6e2dc",
"e9 c #f3f1ea",
"e! c #fbf8f0",
"e# c #fffff8",
"e$ c #eae7e3",
"e% c #e6e3d8",
"e& c #d6d6cc",
"e* c #dbd3ca",
"e+ c #e9e3da",
"e- c #eeebe4",
"e; c #ede9e2",
"e< c #e7e2d9",
"e= c #f1efe7",
"e> c #ebe8e1",
"e? c #f9f7f1",
"e@ c #f0ede8",
"e^ c #eeeedd",
"e_ c #dcdcd0",
"e| c #edeae2",
"e~ c #e1dbd4",
"fa c #f0ebe2",
"fb c #fbf8f1",
"fc c #f6f2eb",
"fd c #e9e6e0",
"fe c #dadada",
"ff c #e5dfd6",
"fg c #f3eee7",
"fh c #fffffb",
"fi c #f4f1ea",
"fj c #e3e3da",
"fk c #bfbfbf",
"fl c #f5f4ee",
"fm c #e4dfdf",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaabacadaeaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaafaaaaaaaaadaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaadaaagahaiajaaacakaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaalaaamanaoapaqarasaaataeaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaalaaauavawaxayazaAaBaCaaaaaDaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaEaaaFaGaHaIaJaKaLaMaNaOaPaQaaaDaRaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aEaaaSaTaUaVaWaXaYaZa0a1a2a3a4a5aaaaa6aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaa7a8a9a!a#a$a%a&a*a+a-a;a<a=a>a?a@aaaea^aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"a_a|a~babbbcbdbebfbgbhbibjbkblbmbnbobpbqaaaDaeaaaaaaaaaaaaaaaaaaaaaaaaaa",
"brbsbtbubvbwbxbybzbAbBbCbDbEbFbGbHbna3bIbJaaaaaDaaaaaaaaaaaaaaaaaaaaaaaa",
"bKbLbMbNbObPbQbRbSbTbUbVbWbXbYbZb0b1b2bib3b4b5aab6b6aaaaaaaaaaaaaaaaaaaa",
"b7b8b9b!b#b$b%b&b*b+b-bDb;b<b=b>b?b@b^b_b|b~cacbaaaaa6abaaaaaaaaaaaaaaaa",
"cccdcecfcgchcicjckclcmcncocpcqcrcsctcucvcwcxcyczcAcBaaaacCaaaaaaaaaaaaaa",
"aacDcEcFcGcHcIcJcKcLcMcNcOcPcQcRcScTcUcVcWcXcYb2cZc0c1c2aaaDaeaaaaaaaaaa",
"c3aac4c5c6c7aaaac8c9c!c#c$c%c&b0c*c+c-c;c<c=c>c?c@c^b~c_c|aaaaa^aaaaaaaa",
"aac~aaaadadbaedcaadddedfdgdhdidjdkdldmdndodpcTdqdrdsdtdudvdwdxaaaeaaaaaa",
"aaaadyc~aaaaaaaab6aeaadzdAdBdCdDcodEdFdGdHdIdJdKdLdMdNdOdPdQdRdSaeb6aaaa",
"aaaaaaaac~abaaaaaaaaa6aaaadTdUdVdWdXdudYcTdZd0d1d2cPd3d4d5d6d7d8aaaDaaaa",
"aaaaaaaaaaaaaaaaaaaaaab6aeaad9d!d#d$d%d&d*d+c;c&d-d;d<d=d>cjd?d@d^aaaeaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaa6aaaad_d|d~eaebecedd~eeefd~egeheiaaejekaeaeaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaRa^aaelemeneoepd=eqereseteuevaaewexaaaaaDaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa6aaaaeyezd~eAeBeCcxeDeEaaeFeGeHeIeJaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaeaDaaeKeLeMeNeOePeQaaeReSeTeUeVeWeXeY",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaDaaaaeZe0e1e2e3e4e5e6e7e8e9e!e#e$",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaeaDaaaae%e&e*e+e-e;e<e=e>e?e@e^",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab6aeaaaaaee_e|e~fafbfcfdfeaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaDaedyaaaafffgfhfifjaab6",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaeaDaafkflfmaaaDaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa^aaaaaaaeaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaeaDaRaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
};
static const char* icon96_xpm[] = {
"96 96 1744 2",
"aa c None",
"ab c #000000",
"ac c #ffff7f",
"ad c #ffffff",
"ae c #7f7f7f",
"af c None",
"ag c #ffffaa",
"ah c None",
"ai c #dbc99a",
"aj c #b7ada3",
"ak c #ffaa55",
"al c #aa927b",
"am c #e9c663",
"an c #fad465",
"ao c #f6dc77",
"ap c #ebd88d",
"aq c #b3aaaa",
"ar c #ffff00",
"as c #aaaa55",
"at c #ffbf7f",
"au c #b69d5b",
"av c #ebc756",
"aw c #ffda53",
"ax c #f7ce51",
"ay c #fed454",
"az c #ffe565",
"aA c #f7e07e",
"aB c #e0cf96",
"aC c None",
"aD c #ffff55",
"aE c #bfbf3f",
"aF c #b9a256",
"aG c #ebc752",
"aH c #ffd752",
"aI c #f5cd54",
"aJ c #f6cd54",
"aK c #f5cc52",
"aL c #f7cd4d",
"aM c #ffd855",
"aN c #ffe570",
"aO c #f3dd88",
"aP c #ddcca7",
"aQ c #c6ac60",
"aR c #eec853",
"aS c #fdd555",
"aT c #f3cc53",
"aU c #f6cc51",
"aV c #f7cc50",
"aW c #f7cb4f",
"aX c #f8cd53",
"aY c #f6cc4f",
"aZ c #f8cf4d",
"a0 c #ffe163",
"a1 c #f8de79",
"a2 c #ead790",
"a3 c #bcb3aa",
"a4 c #cccc33",
"a5 c #c6ac5c",
"a6 c #f0ca51",
"a7 c #fad253",
"a8 c #f3cc53",
"a9 c #f5cc50",
"a! c #f6cc4f",
"a# c #f6cb4d",
"a$ c #f6cd50",
"a% c #f5cc4e",
"a& c #f6cd52",
"a* c #f6cd52",
"a+ c #f9d054",
"a- c #ffe76f",
"a; c #f7df83",
"a< c #e3d091",
"a= c #cc9966",
"a> c #cdaf5a",
"a? c #f1cb50",
"a@ c #f8cf4e",
"a^ c #f2ca4f",
"a_ c #f3ca4e",
"a| c #f2ca51",
"a~ c #f2cb50",
"ba c #f3cb4f",
"bb c #f4cb4d",
"bc c #f5cb4d",
"bd c #f6cc50",
"be c #f6ce53",
"bf c #f7ce55",
"bg c #f6cd51",
"bh c #ffd85b",
"bi c #ffe475",
"bj c #ecd685",
"bk c #cfbf9f",
"bl c #aaaaaa",
"bm c #cc9933",
"bn c #cfad55",
"bo c #f3ca50",
"bp c #f8ce4d",
"bq c #f2c94c",
"br c #f2ca4f",
"bs c #f5cc51",
"bt c #fcd252",
"bu c #f1ca4f",
"bv c #f2ca4d",
"bw c #f5cc4f",
"bx c #f6cc52",
"by c #f7cc53",
"bz c #f6cd53",
"bA c #f6cf55",
"bB c #f6ce54",
"bC c #f5cc53",
"bD c #f9d052",
"bE c #ffe265",
"bF c #f8dd7a",
"bG c #e9d58f",
"bH c #7f7fc8",
"bI c #ffffff",
"bJ c #ffff3f",
"bK c #cbab53",
"bL c #f2ca4d",
"bM c #f9cf4f",
"bN c #f3cb4f",
"bO c #f2c94e",
"bP c #f9cf53",
"bQ c #e3bd4f",
"bR c #78662f",
"bS c #c0a142",
"bT c #fed453",
"bU c #f7ce4f",
"bV c #f0c84e",
"bW c #f3cb52",
"bX c #f5cd53",
"bY c #f5ce53",
"bZ c #f5cd55",
"b0 c #f7ce54",
"b1 c #f7cd52",
"b2 c #fdd358",
"b3 c #ffe673",
"b4 c #f7dc85",
"b5 c #dbc698",
"b6 c #ffbf3f",
"b7 c #d0ae50",
"b8 c #f3cb50",
"b9 c #f8cf51",
"b! c #f3cc51",
"b# c #f2cb52",
"b$ c #f8cf53",
"b% c #eac34d",
"b& c #5a4f2c",
"b* c #10151f",
"b+ c #232321",
"b- c #7d6b36",
"b; c #e0bc4a",
"b< c #ffd552",
"b= c #f5cd52",
"b> c #f6cd51",
"b? c #f5ce54",
"b@ c #f7cc51",
"b^ c #f7ce55",
"b_ c #fad055",
"b| c #fbde92",
"b~ c #f2ebdd",
"ca c #edeae3",
"cb c #c9c9c3",
"cc c None",
"cd c #cfb051",
"ce c #f1cc4c",
"cf c #f7d04f",
"cg c #f3cb50",
"ch c #f1ca4d",
"ci c #f8d04f",
"cj c #efc64d",
"ck c #5b4f2c",
"cl c #171b20",
"cm c #5a4e2b",
"cn c #1a1c1f",
"co c #3e3826",
"cp c #9f853a",
"cq c #ebc54e",
"cr c #f4cd52",
"cs c #f3cb4c",
"ct c #f5cb52",
"cu c #f4cc51",
"cv c #f5ce50",
"cw c #eccf83",
"cx c #e7e1d9",
"cy c #eae7de",
"cz c #fdf7ec",
"cA c #f2eee4",
"cB c #e9e5df",
"cC c #244848",
"cD c #caa84c",
"cE c #f2c84d",
"cF c #f8d050",
"cG c #f2cb4e",
"cH c #f3cc4e",
"cI c #e4be47",
"cJ c #60542c",
"cK c #1a1d22",
"cL c #8d793d",
"cM c #fbd153",
"cN c #f3cc55",
"cO c #766533",
"cP c #171b21",
"cQ c #353222",
"cR c #d8b547",
"cS c #f8ce52",
"cT c #f2cb4f",
"cU c #f3cc4d",
"cV c #f3cb51",
"cW c #f4cc54",
"cX c #f5cc52",
"cY c #e9cc7e",
"cZ c #e6e1da",
"c0 c #e9e5da",
"c1 c #e7e3d6",
"c2 c #e7e3d7",
"c3 c #eeeadd",
"c4 c #fffbf1",
"c5 c #ece9e1",
"c6 c #d8d5d1",
"c7 c #c0a24d",
"c8 c #f1c84d",
"c9 c #f9d052",
"c! c #f3cc52",
"c# c #f4cc4d",
"c$ c #f1ca4c",
"c% c #fbd251",
"c& c #b89a3c",
"c* c #2a2821",
"c+ c #1f2123",
"c- c #776635",
"c; c #e2bd50",
"c< c #c3a548",
"c= c #363327",
"c> c #323027",
"c? c #c5a745",
"c@ c #f8d052",
"c^ c #f0c851",
"c_ c #f3cc52",
"c| c #f5cd51",
"c~ c #f4cd54",
"da c #eaca76",
"db c #e6dfd6",
"dc c #e8e4da",
"dd c #e8e4d7",
"de c #e8e5d9",
"df c #e8e4d8",
"dg c #e7e3d7",
"dh c #e8e3d8",
"di c #f9f3e8",
"dj c #f5f0e7",
"dk c #eceae2",
"dl c #b8b8b8",
"dm c #ffffff",
"dn c #bb9d4c",
"do c #edc64d",
"dp c #f8d04d",
"dq c #f1c94e",
"dr c #f4cb50",
"ds c #f3cb4e",
"dt c #f2c94d",
"du c #face52",
"dv c #d8b449",
"dw c #64562b",
"dx c #11151c",
"dy c #3a3728",
"dz c #353228",
"dA c #272722",
"dB c #c1a345",
"dC c #fcd353",
"dD c #f2c951",
"dE c #f4cd55",
"dF c #f5ce52",
"dG c #ebc971",
"dH c #e6ded0",
"dI c #e8e5da",
"dJ c #e9e4d7",
"dK c #e9e5d9",
"dL c #e8e4d9",
"dM c #e9e4d8",
"dN c #e7e3d7",
"dO c #eae5d9",
"dP c #fef8ef",
"dQ c #efece4",
"dR c #e0ded9",
"dS c #bf7f3f",
"dT c #b89746",
"dU c #eac248",
"dV c #fbd14f",
"dW c #f0c94c",
"dX c #f3c94c",
"dY c #f2c94b",
"dZ c #f2c94f",
"d0 c #f4c94b",
"d1 c #f1c94d",
"d2 c #f9d04f",
"d3 c #f5cd50",
"d4 c #a68e3d",
"d5 c #343122",
"d6 c #242420",
"d7 c #b89c42",
"d8 c #fcd450",
"d9 c #f1ca51",
"d! c #f4cc50",
"d# c #f4cc53",
"d$ c #f7ce52",
"d% c #eccb72",
"d& c #e8dfd3",
"d* c #e8e5db",
"d+ c #e8e3d6",
"d- c #eae5d9",
"d; c #e6e2d6",
"d< c #f0ebe0",
"d= c #fdf9ef",
"d> c #eceae3",
"d? c #cdc9c9",
"d@ c #b1923d",
"d^ c #e8c044",
"d_ c #fcd14d",
"d| c #f0c84a",
"d~ c #f4ca4c",
"ea c #f2c94c",
"eb c #f3ca4b",
"ec c #f3ca4a",
"ed c #f2ca4b",
"ee c #f0c74d",
"ef c #f0c94e",
"eg c #fdd554",
"eh c #d6b349",
"ei c #bfa141",
"ej c #f9d051",
"ek c #f0c94c",
"el c #f4cc4f",
"em c #f4cc4e",
"en c #f3cd52",
"eo c #f6cd4f",
"ep c #ecc96c",
"eq c #e7decf",
"er c #e9e5dd",
"es c #e8e4d6",
"et c #eae4d9",
"eu c #e7e3d7",
"ev c #e6e2d6",
"ew c #eae5d9",
"ex c #fcf5ec",
"ey c #f2eee7",
"ez c #e9e5e0",
"eA c #778888",
"eB c #ff7f7f",
"eC c #a0844b",
"eD c #e8c146",
"eE c #fdd34d",
"eF c #f1c84e",
"eG c #f3cb4a",
"eH c #f2ca4c",
"eI c #efc84f",
"eJ c #fad151",
"eK c #f4cb4f",
"eL c #f3cb4d",
"eM c #f2cb4c",
"eN c #f4cd53",
"eO c #f6ce52",
"eP c #ecc866",
"eQ c #e7ddcc",
"eR c #e9e6de",
"eS c #ede9dc",
"eT c #ede7db",
"eU c #ece7db",
"eV c #f6f1e5",
"eW c #eae6da",
"eX c #e6e3d7",
"eY c #e8e4d8",
"eZ c #eee8dc",
"e0 c #eeebe3",
"e1 c #d8d5cf",
"e2 c #7f7f00",
"e3 c #a38e51",
"e4 c #e6c14a",
"e5 c #fed550",
"e6 c #f2ca4f",
"e7 c #f3ca4d",
"e8 c #f3cb4b",
"e9 c #f3c94d",
"e! c #f3c94f",
"e# c #f1cb4b",
"e$ c #f3cd4c",
"e% c #f4cb4e",
"e& c #f5cd4f",
"e* c #ecc760",
"e+ c #e6dcc9",
"e- c #e8e6dd",
"e; c #e5e2d6",
"e< c #e5e1d6",
"e= c #e8e5d8",
"e> c #dcd7cb",
"e? c #c0bcb1",
"e@ c #44433e",
"e^ c #98958c",
"e_ c #e8e1d5",
"e| c #f2ece0",
"e~ c #e6e1d6",
"fa c #e9e3d8",
"fb c #eae5d8",
"fc c #e9e5d8",
"fd c #e9e2d6",
"fe c #f6efe4",
"ff c #f7f3e9",
"fg c #eae8e1",
"fh c #c3c3bb",
"fi c #ffff00",
"fj c #555571",
"fk c #deba4d",
"fl c #fed54e",
"fm c #f1ca4d",
"fn c #f2ca4a",
"fo c #f2cb49",
"fp c #f3cc4f",
"fq c #f2cb51",
"fr c #ecc65b",
"fs c #e6dac4",
"ft c #e7e4d8",
"fu c #f4efe3",
"fv c #faf6e9",
"fw c #bfbbb2",
"fx c #2b2b28",
"fy c #32312e",
"fz c #171817",
"fA c #1a1b18",
"fB c #32312d",
"fC c #b1aba0",
"fD c #f9f3e6",
"fE c #f5f0e4",
"fF c #ece7dc",
"fG c #eae5da",
"fH c #e7e3d6",
"fI c #ece6db",
"fJ c #fef9ee",
"fK c #f1ede4",
"fL c #dcdcd5",
"fM c #000000",
"fN c #d4ac3c",
"fO c #ffd247",
"fP c #f1c74c",
"fQ c #f3c74b",
"fR c #f3c94b",
"fS c #f3ca4c",
"fT c #f2ca48",
"fU c #f3c84c",
"fV c #f3c84e",
"fW c #ecc658",
"fX c #e5d8bf",
"fY c #e9e2d5",
"fZ c #e9e3d7",
"f0 c #f2eee1",
"f1 c #c8c4b9",
"f2 c #6a6861",
"f3 c #cbc7bb",
"f4 c #4d4d47",
"f5 c #181818",
"f6 c #1d1e1d",
"f7 c #272725",
"f8 c #151515",
"f9 c #1f201d",
"f! c #d4d1c4",
"f# c #b6b2a8",
"f$ c #807e75",
"f% c #e3ded1",
"f& c #fbf8ed",
"f* c #f0ede5",
"f+ c #cdcdc9",
"f- c #a38241",
"f; c #d1a11f",
"f< c #e7b51d",
"f= c #ecc13c",
"f> c #f1c94b",
"f? c #f3c949",
"f@ c #f3c94e",
"f^ c #f4ca4f",
"f_ c #f5cf54",
"f| c #edc856",
"f~ c #e5d7bc",
"ga c #e8e4de",
"gb c #e8e3d4",
"gc c #54534e",
"gd c #0c0d0d",
"ge c #222220",
"gf c #232423",
"gg c #19191a",
"gh c #6c6a64",
"gi c #e1ddd3",
"gj c #696762",
"gk c #9b988f",
"gl c #353533",
"gm c #0d0e0d",
"gn c #2f2f2a",
"go c #cbc6bb",
"gp c #ebe7db",
"gq c #e8e5d8",
"gr c #eae4d9",
"gs c #f9f5e9",
"gt c #f3efe6",
"gu c #eae8e2",
"gv c #c7c7c7",
"gw c #ae842d",
"gx c #c68e1b",
"gy c #cd9007",
"gz c #dda914",
"gA c #e8bb2e",
"gB c #eec644",
"gC c #f1c94c",
"gD c #f3ca49",
"gE c #f2cb4b",
"gF c #f3ca50",
"gG c #f4cd51",
"gH c #f4cc52",
"gI c #edc752",
"gJ c #e4d4b3",
"gK c #e7e4df",
"gL c #e8e2d3",
"gM c #e5e1d5",
"gN c #f1ece0",
"gO c #bcb9af",
"gP c #1d1d1c",
"gQ c #252523",
"gR c #242422",
"gS c #272626",
"gT c #1d1d1d",
"gU c #5b5a54",
"gV c #ccc7bc",
"gW c #efeade",
"gX c #fbf6e9",
"gY c #797770",
"gZ c #1e1e1d",
"g0 c #575551",
"g1 c #efe9dd",
"g2 c #f4f0e4",
"g3 c #e9e5d8",
"g4 c #e8e3d7",
"g5 c #ede8dd",
"g6 c #fffcf1",
"g7 c #f2efe7",
"g8 c #dbdbd3",
"g9 c #bfbfbf",
"g! c #aa8436",
"g# c #ba8622",
"g$ c #b87b0d",
"g% c #c28604",
"g& c #d39907",
"g* c #e1af1a",
"g+ c #ecc034",
"g- c #f0c748",
"g; c #f2cc4e",
"g< c #f2cb4e",
"g= c #f2ca4e",
"g> c #efc74e",
"g? c #e4d2a7",
"g@ c #e6e4df",
"g^ c #e7e1d3",
"g_ c #ebe7da",
"g| c #cbc8bd",
"g~ c #30302e",
"ha c #212121",
"hb c #202020",
"hc c #212120",
"hd c #191a18",
"he c #191a14",
"hf c #d4d0c4",
"hg c #c5c1b7",
"hh c #1e1f1d",
"hi c #232222",
"hj c #272825",
"hk c #d9d4c9",
"hl c #e3dfd3",
"hm c #9a978e",
"hn c #e7e2d7",
"ho c #e7e3d8",
"hp c #e8e3d7",
"hq c #f2ede2",
"hr c #faf6ec",
"hs c #ebe8e2",
"ht c #cacac3",
"hu c #9d7d3c",
"hv c #b08323",
"hw c #b27c14",
"hx c #b27911",
"hy c #bc7e08",
"hz c #ca8e03",
"hA c #daa20b",
"hB c #e7b824",
"hC c #efc542",
"hD c #f1ca4e",
"hE c #f2c950",
"hF c #f2cc51",
"hG c #e6d3a7",
"hH c #e8e3df",
"hI c #e7e2d4",
"hJ c #e4e0d4",
"hK c #f6f0e3",
"hL c #a19d95",
"hM c #343331",
"hN c #363532",
"hO c #090a09",
"hP c #7a7870",
"hQ c #f8f1e6",
"hR c #454541",
"hS c #1f201c",
"hT c #171816",
"hU c #8f8d84",
"hV c #fffcef",
"hW c #6f6e68",
"hX c #0f0f0f",
"hY c #3b3a36",
"hZ c #b0aca2",
"h0 c #f1ebdf",
"h1 c #e8e4d8",
"h2 c #ebe6db",
"h3 c #fcf6eb",
"h4 c #f3f0e5",
"h5 c #e1dfd6",
"h6 c #2a2a2a",
"h7 c #705b3d",
"h8 c #b08223",
"h9 c #b07b10",
"h! c #a8740d",
"h# c #ae7812",
"h$ c #b57a0f",
"h% c #be8107",
"h& c #cf9405",
"h* c #e2ae15",
"h+ c #ecc033",
"h- c #f2c949",
"h; c #f0ca50",
"h< c #f1c748",
"h= c #e6d09a",
"h> c #e7e3df",
"h? c #e7e2d3",
"h@ c #e8e1d6",
"h^ c #e9e2d6",
"h_ c #e7e1d6",
"h| c #eee7db",
"h~ c #d7d2c6",
"ia c #2a2a29",
"ib c #1c1d1c",
"ic c #b8b4aa",
"id c #dfdacf",
"ie c #827f77",
"if c #ece6da",
"ig c #908e87",
"ih c #151514",
"ii c #1e1f1c",
"ij c #4a4844",
"ik c #cac7bc",
"il c #202120",
"im c #171717",
"in c #1c1d1b",
"io c #bdb8af",
"ip c #f0ebdf",
"iq c #e9e4d9",
"ir c #e9e5d9",
"is c #fffaf1",
"it c #eeeae3",
"iu c #dbd7d3",
"iv c #003f3f",
"iw c #aa7e22",
"ix c #af7b11",
"iy c #a6720a",
"iz c #a8740b",
"iA c #aa7510",
"iB c #af7711",
"iC c #b77a0d",
"iD c #c78905",
"iE c #daa20c",
"iF c #e7b724",
"iG c #eec442",
"iH c #f1c747",
"iI c #e6cf93",
"iJ c #e8e3de",
"iK c #e9e3d5",
"iL c #e8e2d6",
"iM c #e6e0d4",
"iN c #f4eee1",
"iO c #62615c",
"iP c #171818",
"iQ c #716f68",
"iR c #f7f1e3",
"iS c #fef8ea",
"iT c #cdcac0",
"iU c #232322",
"iV c #20201f",
"iW c #c1bdb3",
"iX c #f5f1e5",
"iY c #f1ede1",
"iZ c #d4cfc6",
"i0 c #5d5c57",
"i1 c #8d8a82",
"i2 c #f7f0e5",
"i3 c #e2ded2",
"i4 c #faf4e9",
"i5 c #f3efe5",
"i6 c #e6e2da",
"i7 c #aaaa9d",
"i8 c #a17722",
"i9 c #b27c11",
"i! c #a4710b",
"i# c #a6720b",
"i$ c #a5720b",
"i% c #a7740e",
"i& c #ac7813",
"i* c #b2790f",
"i+ c #c08405",
"i- c #d19705",
"i; c #e0ad17",
"i< c #ecc132",
"i= c #f0c849",
"i> c #f2cb4e",
"i? c #f0c744",
"i@ c #e7cd8b",
"i^ c #e6e2de",
"i_ c #e9e2d3",
"i| c #e8e2d5",
"i~ c #e7e2d6",
"ja c #e5e0d4",
"jb c #f6f0e4",
"jc c #a6a399",
"jd c #181918",
"je c #353531",
"jf c #e0dbcf",
"jg c #eae6d9",
"jh c #e4dfd2",
"ji c #eee9dd",
"jj c #555551",
"jk c #1c1c1c",
"jl c #171716",
"jm c #7b7972",
"jn c #fffff4",
"jo c #8d8b83",
"jp c #605f58",
"jq c #dad4c9",
"jr c #f3eee2",
"js c #f4ede2",
"jt c #ebe6da",
"ju c #eee8dd",
"jv c #fffaef",
"jw c #f1ede5",
"jx c #e2dfda",
"jy c #ffffff",
"jz c #00ffff",
"jA c #9d7222",
"jB c #af7a0e",
"jC c #a07009",
"jD c #a1700a",
"jE c #a37109",
"jF c #a87510",
"jG c #b07911",
"jH c #ba7e0b",
"jI c #c78b06",
"jJ c #d8a10c",
"jK c #e6b621",
"jL c #edc33e",
"jM c #f8ce4c",
"jN c #efd388",
"jO c #e6e2dc",
"jP c #e7e2d5",
"jQ c #e7e2d5",
"jR c #e7e1d5",
"jS c #d8d4c8",
"jT c #343330",
"jU c #222222",
"jV c #161717",
"jW c #a8a39b",
"jX c #e0dcd0",
"jY c #f6f1e3",
"jZ c #9a978f",
"j0 c #171718",
"j1 c #3d3d39",
"j2 c #ebe7dc",
"j3 c #d1cdc2",
"j4 c #252524",
"j5 c #111111",
"j6 c #21201d",
"j7 c #d0cbc0",
"j8 c #b2ada3",
"j9 c #ebe3d7",
"j! c #ece8dc",
"j# c #eae7da",
"j$ c #eae5da",
"j% c #f5f0e5",
"j& c #fbf7ee",
"j* c #ebe9df",
"j+ c #c7c7c0",
"j- c None",
"j; c #86632a",
"j< c #97690e",
"j= c #a06e04",
"j> c #9b6b07",
"j? c #9d6d07",
"j@ c #9d6e08",
"j^ c #a17009",
"j_ c #a3720a",
"j| c #a6740d",
"j~ c #ad7811",
"ka c #b37a0f",
"kb c #be8206",
"kc c #cb9104",
"kd c #ecb71b",
"ke c #d9b342",
"kf c #ddd7d4",
"kg c #fbf4e6",
"kh c #e8e2d6",
"ki c #e6e0d4",
"kj c #e8e1d3",
"kk c #f5f0e3",
"kl c #787770",
"km c #070708",
"kn c #111213",
"ko c #5e5d58",
"kp c #e5dfd3",
"kq c #d9d5c9",
"kr c #2c2d2a",
"ks c #1a1a19",
"kt c #b3afa5",
"ku c #fdf9ec",
"kv c #55544f",
"kw c #1b1c1b",
"kx c #1c1c1d",
"ky c #716f69",
"kz c #fefcee",
"kA c #88867f",
"kB c #0a0a0a",
"kC c #1b1a18",
"kD c #55534f",
"kE c #b8b4ab",
"kF c #f3efe2",
"kG c #eae4d8",
"kH c #ebe4d8",
"kI c #ebe4d7",
"kJ c #ebe6da",
"kK c #eae5d9",
"kL c #ece6da",
"kM c #f2efe6",
"kN c #e5e5df",
"kO c #7f667f",
"kP c #755313",
"kQ c #8c6005",
"kR c #9c6c03",
"kS c #986a05",
"kT c #976904",
"kU c #9b6c06",
"kV c #9f6f08",
"kW c #a37209",
"kX c #a4720a",
"kY c #a8750e",
"kZ c #ae7810",
"k0 c #b87d07",
"k1 c #c48e0d",
"k2 c #957b3d",
"k3 c #00007f",
"k4 c #ded5c8",
"k5 c #e7e1d6",
"k6 c #e8e0d3",
"k7 c #e7dfd1",
"k8 c #e7dfd2",
"k9 c #e8e1d4",
"k! c #dcd8cc",
"k# c #94918a",
"k$ c #454440",
"k% c #d1ccc0",
"k& c #f1e9de",
"k* c #e7dfd4",
"k+ c #65635f",
"k- c #191a1a",
"k; c #181919",
"k< c #67665f",
"k= c #fffff1",
"k> c #9c9991",
"k? c #171715",
"k@ c #373634",
"k^ c #e9e6d9",
"k_ c #cfcbc1",
"k| c #262626",
"k~ c #1b1b1b",
"la c #111112",
"lb c #191917",
"lc c #56534e",
"ld c #b1aea4",
"le c #ebe6db",
"lf c #f1ebe1",
"lg c #fffcf3",
"lh c #ecebe3",
"li c #d9d9d2",
"lj c #4d3607",
"lk c #7c5404",
"ll c #926302",
"lm c #976802",
"ln c #966803",
"lo c #9a6a05",
"lp c #9f6f07",
"lq c #a27209",
"lr c #a37208",
"ls c #a4720a",
"lt c #b78213",
"lu c #a5740f",
"lv c #ffaa00",
"lw c #cac1b8",
"lx c #e2dbd0",
"ly c #f5eee0",
"lz c #ede6d9",
"lA c #e7dfd2",
"lB c #ece5d8",
"lC c #f4efe2",
"lD c #ece7da",
"lE c #e5dfd4",
"lF c #f5eee2",
"lG c #a39f97",
"lH c #232223",
"lI c #2d2d2b",
"lJ c #dedacf",
"lK c #dcd8cd",
"lL c #2c2c2b",
"lM c #a9a69d",
"lN c #fcf9ec",
"lO c #545350",
"lP c #1b1a1b",
"lQ c #1f1f1f",
"lR c #545451",
"lS c #343433",
"lT c #0d0d0e",
"lU c #494843",
"lV c #ece6da",
"lW c #ebe5da",
"lX c #f7f3e7",
"lY c #f7f5ed",
"lZ c #ece9e4",
"l0 c #bab0b0",
"l1 c #555500",
"l2 c #6c4805",
"l3 c #825803",
"l4 c #996902",
"l5 c #976704",
"l6 c #996805",
"l7 c #9e6d06",
"l8 c #9e6e07",
"l9 c #b07c0f",
"l! c #9c6c10",
"l# c #cc6600",
"l$ c #d7d1c7",
"l% c #e8e0d4",
"l& c #eee7d9",
"l* c #e7e1d4",
"l+ c #e6e1d3",
"l- c #e8e0d3",
"l; c #e5ded1",
"l< c #4a4945",
"l= c #101012",
"l> c #121313",
"l? c #9e9a92",
"l@ c #fffef1",
"l^ c #63625d",
"l_ c #181819",
"l| c #5f5d59",
"l~ c #3f3f3c",
"ma c #efeadf",
"mb c #d8d3c9",
"mc c #7f7c77",
"md c #66635e",
"me c #d7d2c7",
"mf c #c0bdb3",
"mg c #f7f1e5",
"mh c #e9e5d9",
"mi c #ede8de",
"mj c #fffbf2",
"mk c #efebe3",
"ml c #e3e1d9",
"mm c #7f0000",
"mn c #ff7f00",
"mo c #4c3308",
"mp c #785204",
"mq c #906202",
"mr c #9a6903",
"ms c #976703",
"mt c #9b6b05",
"mu c #9b6c05",
"mv c #a9780c",
"mw c #95690c",
"mx c #bf7f00",
"my c #9c8989",
"mz c #dbd4c9",
"mA c #f3eddf",
"mB c #e6e0d3",
"mC c #e8e0d4",
"mD c #dfdbcf",
"mE c #85827c",
"mF c #686661",
"mG c #faf4e7",
"mH c #a8a49c",
"mI c #212222",
"mJ c #272726",
"mK c #ddd9ce",
"mL c #2d2e2c",
"mM c #212020",
"mN c #1b1b1a",
"mO c #a29e97",
"mP c #111110",
"mQ c #4f4d48",
"mR c #fffbee",
"mS c #7f7d77",
"mT c #121211",
"mU c #615e58",
"mV c #c9c4ba",
"mW c #ede9dd",
"mX c #ece6db",
"mY c #eae6db",
"mZ c #e9e4d9",
"m0 c #f3eee4",
"m1 c #eeebe2",
"m2 c #cecec5",
"m3 c #7f3f00",
"m4 c #644405",
"m5 c #855a03",
"m6 c #9a6a03",
"m7 c #966603",
"m8 c #976703",
"m9 c #a47309",
"m! c #8f650b",
"m# c #d1cec2",
"m$ c #e7e0d5",
"m% c #f5efe1",
"m& c #e8e2d4",
"m* c #e8dfd2",
"m+ c #e9e1d4",
"m- c #ede8dc",
"m; c #504e4b",
"m< c #0d0e0f",
"m= c #928e87",
"m> c #fffff2",
"m? c #161617",
"m@ c #6e6d67",
"m^ c #f4f0e3",
"m_ c #d4d1c6",
"m| c #272727",
"m~ c #1f1f20",
"na c #d0cbc2",
"nb c #201f20",
"nc c #242424",
"nd c #121212",
"ne c #bab4ab",
"nf c #f1ede0",
"ng c #f5f1e4",
"nh c #f2ece1",
"ni c #ebe5d9",
"nj c #fcf6ec",
"nk c #f2ede4",
"nl c #e9e5dd",
"nm c #aaaaaa",
"nn c #3f2a0a",
"no c #795201",
"np c #906302",
"nq c #986802",
"nr c #9e7005",
"ns c #8d620a",
"nt c #b6aa9d",
"nu c #e0d9cd",
"nv c #ebe5d8",
"nw c #f1ebde",
"nx c #e7e0d4",
"ny c #e7dfd3",
"nz c #e6e1d4",
"nA c #e0dcd1",
"nB c #5d5b56",
"nC c #30302d",
"nD c #dcd7cc",
"nE c #f3eee1",
"nF c #64635d",
"nG c #151616",
"nH c #78766f",
"nI c #fef8ec",
"nJ c #4e4d49",
"nK c #181718",
"nL c #f9f4e7",
"nM c #e3e0d4",
"nN c #89867f",
"nO c #eae6da",
"nP c #f0e8df",
"nQ c #eeece2",
"nR c #d4cfc9",
"nS c #aa5500",
"nT c #684802",
"nU c #825902",
"nV c #9e6d02",
"nW c #8b6105",
"nX c #000033",
"nY c #ccc5bd",
"nZ c #e6dfd3",
"n0 c #e8dfd3",
"n1 c #e6e1d5",
"n2 c #51504d",
"n3 c #0e0f10",
"n4 c #121113",
"n5 c #a29f97",
"n6 c #f2ede1",
"n7 c #f1ecdf",
"n8 c #afaca4",
"n9 c #1e1d1e",
"n! c #343532",
"n# c #f2ede0",
"n$ c #98958d",
"n% c #3d3c3a",
"n& c #a9a59d",
"n* c #101011",
"n+ c #191818",
"n- c #79766f",
"n; c #eae6db",
"n< c #f6f2e8",
"n= c #fdf8ee",
"n> c #ece9e1",
"n? c #b8afa7",
"n@ c #331e00",
"n^ c #7d5603",
"n_ c #815a05",
"n| c #553c0c",
"n~ c #ff0000",
"oa c #dbd3c8",
"ob c #ede6da",
"oc c #e7e1d5",
"od c #6b6863",
"oe c #eee9dc",
"of c #3c3c38",
"og c #b1aea5",
"oh c #e3ded4",
"oi c #262726",
"oj c #232323",
"ok c #1a1a1a",
"ol c #b0aea5",
"om c #363635",
"on c #242425",
"oo c #18191a",
"op c #9e9b93",
"oq c #f0eadd",
"or c #cac5bb",
"os c #e9e6da",
"ot c #e9e5db",
"ou c #ede7de",
"ov c #fffcf2",
"ow c #f1eee4",
"ox c #dddad0",
"oy c #614004",
"oz c #55380e",
"oA c #d2cabf",
"oB c #e4dfd2",
"oC c #f6f2e5",
"oD c #e7e2d6",
"oE c #e5e1d4",
"oF c #f5efe2",
"oG c #6f6d68",
"oH c #5f5e5a",
"oI c #65625f",
"oJ c #65635e",
"oK c #e0ddd1",
"oL c #f7f2e5",
"oM c #7b7873",
"oN c #1b1c1c",
"oO c #5c5b57",
"oP c #fffdef",
"oQ c #777570",
"oR c #090a0b",
"oS c #1e1e1e",
"oT c #797670",
"oU c #f0ecdf",
"oV c #ebe5db",
"oW c #ece2db",
"oX c #f5eee4",
"oY c #fefaef",
"oZ c #eeede4",
"o0 c #d5d1cc",
"o1 c #ada296",
"o2 c #dfdace",
"o3 c #efeadd",
"o4 c #e6e2d5",
"o5 c #e8e3d8",
"o6 c #929088",
"o7 c #2e2e2d",
"o8 c #bcb8af",
"o9 c #0f1012",
"o! c #353432",
"o# c #e6e0d5",
"o$ c #c3c0b5",
"o% c #222221",
"o& c #282827",
"o* c #cfcac0",
"o+ c #96948d",
"o- c #313131",
"o; c #1d1e1f",
"o< c #a7a39c",
"o= c #f0ece0",
"o> c #e2ded3",
"o? c #efebdf",
"o@ c #ede7dc",
"o^ c #ece5dc",
"o_ c #faf5ec",
"o| c #fbf8ef",
"o~ c #e3e1da",
"pa c #d2cec0",
"pb c #e5e0d4",
"pc c #d7d3c8",
"pd c #bcb7ae",
"pe c #e1ddd2",
"pf c #8a8882",
"pg c #1a1b1b",
"ph c #383735",
"pi c #a6a299",
"pj c #4e4e4a",
"pk c #908c85",
"pl c #cac6bc",
"pm c #252525",
"pn c #252626",
"po c #a9a59c",
"pp c #9e9c94",
"pq c #d6d2c6",
"pr c #ede5db",
"ps c #ece3db",
"pt c #efe9de",
"pu c #fffcf4",
"pv c #e3e0d9",
"pw c #b5b5ae",
"px c #330000",
"py c #d8d1c6",
"pz c #e8e2d6",
"pA c #e5e2d6",
"pB c #e6e2d6",
"pC c #e3ded2",
"pD c #f8f4e6",
"pE c #090a0c",
"pF c #141413",
"pG c #4e4c48",
"pH c #73706b",
"pI c #1d1d1e",
"pJ c #4a4845",
"pK c #a3a199",
"pL c #2e2e2e",
"pM c #a4a198",
"pN c #b6b2a9",
"pO c #918f88",
"pP c #3c3c39",
"pQ c #101010",
"pR c #1c1c1a",
"pS c #a5a198",
"pT c #f6f1e4",
"pU c #eeeae1",
"pV c #fffffc",
"pW c #d5d2cc",
"pX c #c3b7b1",
"pY c #e5ddd2",
"pZ c #e7e1d4",
"p0 c #d4d0c5",
"p1 c #898680",
"p2 c #30302f",
"p3 c #bab6ad",
"p4 c #f9f4e8",
"p5 c #ded9cd",
"p6 c #f8f3e6",
"p7 c #595856",
"p8 c #282828",
"p9 c #bab5ad",
"p! c #0a0b0d",
"p# c #272728",
"p$ c #302f2f",
"p% c #d0ccc1",
"p& c #ede6db",
"p* c #ebe5da",
"p+ c #f4f0e8",
"p- c #dad4cb",
"p; c #ffbfbf",
"p< c #d6cfc3",
"p= c #e8e0d5",
"p> c #f6efe3",
"p? c #e7e1d5",
"p@ c #f8f1e4",
"p^ c #ddd8cd",
"p_ c #85827d",
"p| c #1b1b1c",
"p~ c #0f1010",
"qa c #6c6a65",
"qb c #f9f6e9",
"qc c #c8c3b8",
"qd c #ded8cc",
"qe c #4c4b48",
"qf c #191819",
"qg c #2c2c2d",
"qh c #313130",
"qi c #696763",
"qj c #d8d3ca",
"qk c #ebe6da",
"ql c #faf5ea",
"qm c #e7e4d9",
"qn c #ccc6ba",
"qo c #636355",
"qp c #737373",
"qq c #c5bdb4",
"qr c #e2ded1",
"qs c #e6e1d5",
"qt c #ddd9cd",
"qu c #817f79",
"qv c #3c3b38",
"qw c #e0dacf",
"qx c #c1bdb4",
"qy c #3d3c39",
"qz c #060609",
"qA c #6b6a65",
"qB c #a8a59e",
"qC c #e7e3d9",
"qD c #d9d5ca",
"qE c #f3efe3",
"qF c #ebe8dc",
"qG c #ebe4d9",
"qH c #e6e1d7",
"qI c #94947f",
"qJ c #bcb9b3",
"qK c #bbb8b4",
"qL c #dad3c9",
"qM c #e7e3d6",
"qN c #e6e1d4",
"qO c #eae3d8",
"qP c #0c0c0d",
"qQ c #5a5855",
"qR c #918f89",
"qS c #464543",
"qT c #64625d",
"qU c #f0ebde",
"qV c #ece7de",
"qW c #eae5db",
"qX c #f7f1e7",
"qY c #e3dfd7",
"qZ c #777777",
"q0 c #cac6c0",
"q1 c #c7c5be",
"q2 c #898989",
"q3 c #b9b1aa",
"q4 c #ddd8cc",
"q5 c #efeadc",
"q6 c #f1eadc",
"q7 c #e9e3d6",
"q8 c #eae3d7",
"q9 c #d6d2c7",
"q! c #7d7b76",
"q# c #3a3a38",
"q$ c #585753",
"q% c #63615c",
"q& c #f9f3e9",
"q* c #e8e3db",
"q+ c #999999",
"q- c #d4d4d4",
"q; c #d0cec7",
"q< c #bcbab4",
"q= c #484848",
"q> c #d3d0c4",
"q? c #e7e0d4",
"q@ c #f8f0e3",
"q^ c #e7e1d5",
"q_ c #e7e1d5",
"q| c #555451",
"q~ c #595752",
"ra c #c9c5ba",
"rb c #eae4d8",
"rc c #f4efe4",
"rd c #eee7df",
"re c #cec8bc",
"rf c #cccccc",
"rg c #dedbd6",
"rh c #cfccc6",
"ri c #a39e9b",
"rj c #bdbdb8",
"rk c #ded6cb",
"rl c #eee7da",
"rm c #e7e2d5",
"rn c #e7e2d6",
"ro c #111011",
"rp c #b0aea4",
"rq c #f2eee2",
"rr c #f2ebe0",
"rs c #f1eae3",
"rt c #c8c3b9",
"ru c #555555",
"rv c #d0ccc8",
"rw c #dcd9d4",
"rx c #878282",
"ry c #a4a49e",
"rz c #d4cac0",
"rA c #e7e1d4",
"rB c #e7e2d7",
"rC c #e7e4d7",
"rD c #ede8dd",
"rE c #dbd6cb",
"rF c #6c6965",
"rG c #7c7a74",
"rH c #ebe4d9",
"rI c #ece4d9",
"rJ c #ebe5d9",
"rK c #e9e5d9",
"rL c #f1eadf",
"rM c #f1ebe1",
"rN c #cbc4bd",
"rO c #acac9f",
"rP c #b2ada8",
"rQ c #bfb9b6",
"rR c #8d8d8d",
"rS c #7f7f6d",
"rT c #dad2c8",
"rU c #e6e1d5",
"rV c #f0e9dd",
"rW c #ece4d9",
"rX c #eee8de",
"rY c #faf3ea",
"rZ c #d9d2c7",
"r0 c #ffffda",
"r1 c #b0aca4",
"r2 c #bcb7b2",
"r3 c #d6d1d1",
"r4 c #c5c0c0",
"r5 c #82827c",
"r6 c #73735c",
"r7 c #bbb2aa",
"r8 c #e4ddd2",
"r9 c #f7f1e4",
"r! c #ede5db",
"r# c #fdf5ec",
"r$ c #dbd5cb",
"r% c #dadada",
"r& c #b1aaa2",
"r* c #e0dcd6",
"r+ c #dad4cf",
"r- c #cdc7c1",
"r; c #dcd8d4",
"r< c #d1cecc",
"r= c #242424",
"r> c #ffffbf",
"r? c #550000",
"r@ c #dad3c7",
"r^ c #e7e3d6",
"r_ c #e8e3d7",
"r| c #ebe6da",
"r~ c #ddd5cd",
"sa c #a69c93",
"sb c #dbd5ce",
"sc c #eae5df",
"sd c #ddd9d2",
"se c #eae5df",
"sf c #dfd9d3",
"sg c #c6c2bc",
"sh c #e1e0db",
"si c #e1e1e1",
"sj c #bcb6aa",
"sk c #e2ded2",
"sl c #e6e2d6",
"sm c #eae5d8",
"sn c #dedad0",
"so c #ffffff",
"sp c #adada2",
"sq c #dfd9d1",
"sr c #f2ede6",
"ss c #ede9e4",
"st c #e3e1d9",
"su c #f0ede5",
"sv c #e5e1da",
"sw c #837875",
"sx c #c2bdb6",
"sy c #e3e0db",
"sz c #dfddda",
"sA c #dbd6c9",
"sB c #e8e4d8",
"sC c #e7e1d6",
"sD c #e0dad2",
"sE c #000000",
"sF c #e1d9d2",
"sG c #eeeae2",
"sH c #eae7e1",
"sI c #eae7df",
"sJ c #edebe3",
"sK c #e6e0da",
"sL c #c1bdb9",
"sM c #5f5555",
"sN c #d0c7bd",
"sO c #dcd5cf",
"sP c #dcd9d6",
"sQ c #dcdcdc",
"sR c #ffaaaa",
"sS c #bdb7ab",
"sT c #e0dbcf",
"sU c #f3ede0",
"sV c #e7e2d6",
"sW c #e6e3d6",
"sX c #f8f3e8",
"sY c #e4ded6",
"sZ c #333333",
"s0 c #c0b8b2",
"s1 c #dad4ce",
"s2 c #d3cbc6",
"s3 c #e0dbd2",
"s4 c #f5f1e9",
"s5 c #eceae2",
"s6 c #f2eee4",
"s7 c #ebe6e1",
"s8 c #d7d7d1",
"s9 c #c0b5b1",
"s! c #ddd6d0",
"s# c #e7e4dd",
"s$ c #eae8e4",
"s% c #919191",
"s& c #d8d2c8",
"s* c #e6dfd4",
"s+ c #e8e2d5",
"s- c #f7f2e6",
"s; c #e7e1d9",
"s< c #8b8b73",
"s= c #ffffd4",
"s> c #d4cdc4",
"s? c #e9e2db",
"s@ c #7f7979",
"s^ c #cac0bb",
"s_ c #b9b3ad",
"s| c #e8e4dc",
"s~ c #f2f0e9",
"ta c #efebe4",
"tb c #cac3bc",
"tc c #dfdfdf",
"td c #dcd9d2",
"te c #f6f1ea",
"tf c #ede7df",
"tg c #f6f2eb",
"th c #eeebe6",
"ti c #ddd9d9",
"tj c #d9d4c9",
"tk c #eae3d6",
"tl c #f1e9de",
"tm c #ebe4db",
"tn c #bcb6b0",
"to c #ffffcc",
"tp c #c2bbb1",
"tq c #eae1d9",
"tr c #f2eee7",
"ts c #b4b1a9",
"tt c #c7bfb9",
"tu c #9c9890",
"tv c #d3cfc9",
"tw c #f7f3ec",
"tx c #eae4dd",
"ty c #e9e4dc",
"tz c #dcd6cf",
"tA c #dedbd1",
"tB c #f6f4ec",
"tC c #f2efe8",
"tD c #eeeae1",
"tE c #eae4db",
"tF c #f2e9e0",
"tG c #f6f1eb",
"tH c #ecebe7",
"tI c #ddd2d2",
"tJ c #afa79e",
"tK c #e4dcd0",
"tL c #ede7da",
"tM c #e7e2d5",
"tN c #e8e2d5",
"tO c #eeeadf",
"tP c #c3beb4",
"tQ c #d0c9c0",
"tR c #ede5dc",
"tS c #eee7df",
"tT c #f2ebe4",
"tU c #ebe4dd",
"tV c #e7e1d7",
"tW c #e4dfd7",
"tX c #d5cfc8",
"tY c #d5cfc7",
"tZ c #f2e9e3",
"t0 c #fffef8",
"t1 c #efebe4",
"t2 c #e7e3dd",
"t3 c #eeebe4",
"t4 c #f7f4ed",
"t5 c #eee9e2",
"t6 c #f1eee6",
"t7 c #ede9e2",
"t8 c #efeae4",
"t9 c #fffffb",
"t! c #e9e7e2",
"t# c #a5a5a5",
"t$ c #ccc4bc",
"t% c #e3ddd2",
"t& c #f2ebe0",
"t* c #f2ece3",
"t+ c #f5f1e7",
"t- c #d0cdc3",
"t; c #d3cbc1",
"t< c #fbf3ea",
"t= c #f1e8e1",
"t> c #f0e7df",
"t? c #e1dad2",
"t@ c #eee9e1",
"t^ c #f0ede2",
"t_ c #f9f5ed",
"t| c #e6e1d8",
"t~ c #e2dcd2",
"ua c #d1cdc5",
"ub c #e6e1d9",
"uc c #f8f6ef",
"ud c #fcfaf2",
"ue c #efede6",
"uf c #f1eee7",
"ug c #f0ece5",
"uh c #f2eee9",
"ui c #f0ece6",
"uj c #efece5",
"uk c #f0ece6",
"ul c #f6f3ee",
"um c #e3e0db",
"un c #7f7fff",
"uo c #d1cec2",
"up c #e5e0d5",
"uq c #eceae2",
"ur c #d7d3ca",
"us c #d4cdc2",
"ut c #f1e9de",
"uu c #f2ede5",
"uv c #efe9e0",
"uw c #b7b0aa",
"ux c #dbd5cb",
"uy c #fdf9f2",
"uz c #f4f0e9",
"uA c #e7e2da",
"uB c #ebe6de",
"uC c #f2eee8",
"uD c #e1ddd6",
"uE c #dcd7d2",
"uF c #f2eee8",
"uG c #f8f4ed",
"uH c #f1ede6",
"uI c #f4f1ea",
"uJ c #f1eee7",
"uK c #efece5",
"uL c #fffcf7",
"uM c #e6e3e1",
"uN c #555555",
"uO c #7f7f7f",
"uP c #dbd0c6",
"uQ c #f0eae1",
"uR c #e3dfd7",
"uS c #d2cac5",
"uT c #f5f1ea",
"uU c #fffcf6",
"uV c #e7e4de",
"uW c #3f3f3f",
"uX c #d9d5cd",
"uY c #ece7e0",
"uZ c #f1ece6",
"u0 c #faf6ef",
"u1 c #ece9e2",
"u2 c #d8d4ce",
"u3 c #e8e4dc",
"u4 c #f8f5ee",
"u5 c #efece6",
"u6 c #f2efe8",
"u7 c #fffdf7",
"u8 c #e4e2e2",
"u9 c #c3bdb1",
"u! c #eee8e1",
"u# c #f2f0e8",
"u$ c #f6f2eb",
"u% c #e9e7e2",
"u& c #7f7f7f",
"u* c #9a9292",
"u+ c #eee8e1",
"u- c #f2ede6",
"u; c #f9f6ef",
"u< c #f6f3ec",
"u= c #e8e6e1",
"u> c #e1dcd6",
"u? c #e8e5df",
"u@ c #f7f4ee",
"u^ c #e6e6e3",
"u_ c #e2dcd4",
"u| c #ece8df",
"u~ c #dbd3d0",
"va c #e8e5dd",
"vb c #f6f1eb",
"vc c #ece9e2",
"vd c #efede7",
"ve c #f1ede7",
"vf c #fffbf3",
"vg c #f1ede5",
"vh c #cbc6bf",
"vi c #eae5de",
"vj c #eeebe6",
"vk c #7f7f7f",
"vl c #bbbbb4",
"vm c #e9e4db",
"vn c #f0ebe1",
"vo c #e4dfd6",
"vp c #bfbfb4",
"vq c #e4dfd8",
"vr c #fffef6",
"vs c #f1ede5",
"vt c #efe9e2",
"vu c #f1ece6",
"vv c #f1ece7",
"vw c #efeae3",
"vx c #fffbf4",
"vy c #e3e0db",
"vz c #c4b0b0",
"vA c #ded8d0",
"vB c #ebe7dd",
"vC c #ede9e0",
"vD c #f3ede7",
"vE c #f0e9e2",
"vF c #f2eee5",
"vG c #f2ede5",
"vH c #f2ece6",
"vI c #f1ece5",
"vJ c #eee9e2",
"vK c #f8f3ed",
"vL c #efedeb",
"vM c #cad3ca",
"vN c #e7e3d9",
"vO c #f4f0e7",
"vP c #f6f1ea",
"vQ c #eee8e1",
"vR c #f0ebe6",
"vS c #f2eee7",
"vT c #eeeae2",
"vU c #f6f0e9",
"vV c #f5f2ec",
"vW c #d2d2d2",
"vX c #d6d6cc",
"vY c #f3eee6",
"vZ c #f2eee7",
"v0 c #f4efe7",
"v1 c #f7f4ef",
"v2 c #e3dfdf",
"v3 c #b4b4a5",
"v4 c #e9e6db",
"v5 c #f5f2ea",
"v6 c #fbf9f1",
"v7 c #f7f4ef",
"v8 c #d9d6d9",
"v9 c #d3ccc4",
"v! c #f2eeeb",
"v# c #e1dfdd",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaabacadaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaeafaaaaagaeaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaacaaahaiajaaaaagaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaakaaalamanaoapaqaaarasaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaataaauavawaxayazaAaBaCaaaDadaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaEaaaFaGaHaIaJaKaLaMaNaOaPaaaaagaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaataaaQaRaSaTaUaVaWaXaYaZa0a1a2a3aaaracaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaa4aaa5a6a7a8a9a!a!a#a$a%a&a*a+a-a;a<aaaaatadaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaa=aaa>a?a@a^a_a|a~babbbcbda&bebfbgbhbibjbkaaaablaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaabmaabnbobpbqbrbsaybtbubvbwbxbybzbAbBbCbDbEbFbGbHaabIaeaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaabJaabKbLbMbNbObPbQbRbSbTbUbVbWbXbYbZbBb0b0b1b2b3b4b5aaaabIabaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaab6aab7b8b9b!b#b$b%b&b*b+b-b;b<b=a~bsbsb>b?bzb@b^b_b|b~cacbaaccblaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaab6aacdcecfcgchcicjckclb-cmcncocpcqcrcsa9ctcucuaJcvcwcxcyczcAcBcCaabIadaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaa4aacDcEcFcGcHbbcIcJcKcLcMcNcOcPcQcRcScTcUb8cVcWcXcYcZc0c1c2c3c4c5c6aaaabIaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaEaac7c8c9c!c#c$c%c&c*c+c-c;c<c=c>c?c@c^c_c|cuc~a!dadbdcdddedfdgdhdidjdkdlaaaddmaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaEaadndodpdqdrdscsdtdudvdwdxdydzdAdBdCdDcucubXdEdFdGdHdIdJdKdedLdMdfdNdOdPdQdRaaaabIadaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaadSaadTdUdVdWdXdYdZdXd0d1d2d3d4d5d6d7d8d9crd!d!d#d$d%d&d*d+dfdfdfdKdMdKd-dfd;d<d=d>d?aaaabIaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaakaad@d^d_d|d~dXeabvebecedeeefegeheiejekelbaemeneoepeqeresetdMdfeudMdfevdMdfdfdgewexeyezeAaabIaeaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaeBaaeCeDeEeFeGebeHa_dsa_eddtbObreIa$eJbOeKeLeMeNeOePeQeResdfdfdfeueSeTeUeVeWeXdfdfdKeYeZc4e0e1aaaabIaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"e2afe3e4e5e6a_e7e7eHe8e8a_e7e9e!badre7e#e$e%a_a~e&e*e+e-c1dfeve;e<e=e>e?e@e^e_e|e~e;fafbfcfdfefffgfhaaaddmaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"fifjfkflfmdsbveaeafnfoe8eddteLfpemelcXe$eMbvfqa9frfseRd+fteueUfufvfwfxfyfzfAfBfCfDfEfFdddKfGfHfIfJfKfLfMaabIadaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aafNfOfPfQfRfSececfnfTecfUfVd!elememc|d!a_b8a$fWfXeRfYfZe~f0f1f2f3f4f5f6f7f8f9f!f#f$f%eUeuc0dKdfd;d<f&f*f+aaaabIaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"f-f;f<f=f>eadYedf?fSeLeMf@f^d!bsd!a%eld!bWf_f|f~gagbeufZdMdKgcgdgegfggghgigjgkfuglgmgngogpeueWdKeWgqgrgsgtgugvaadmaeaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"gwgxgygzgAgBf>gCgDgEgFbaeMgGgHdsemelcUfpcvgIgJgKgLfZfZgMgNgOgPgQgRgSgTgUgVgWgXgYf5gZg0g1g2g3g4eWeWdMfZfHg5g6g7g8aaaag9abaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"g!g#g$g%g&g*g+g-eaece9a_b8cucuc|g;g<g=gHg>g?g@g^g4fZfZg4evg_g|g~hahbhchdhehfhghhhihjhkhlhmfaf0hndMg4eudfhohphqhrhshtaaaablaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"huhvhwhxhyhzhAhBhChDe8bvhEcWc|crhFbrd#a|hGhHhIg4g4g4fZfZhJhKhLf6hbhMhNhOhPhQhRhShThUhVhWhXhYhZh0e~dfeudfeudLh1h2h3h4h5h6aabIadaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"h7h8h9h!h#h$h%h&h*h+h-g<g=bWcWbWh;gHh<h=h>h?h@h^g4fZfZh_h|h~iahaibicidieifigihiiijf0ikiliminioipevdfiqdKdfe=gpgpird<isitiuaaaabIaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"iviwixiyiziAiBiCiDiEiFiGcTbrbWfqfqiHiIiJiKiLiLiLg4eug4iMiNiOggiPiQiRfYiSiTgfiUiViWiXiYiZi0i1i2i3dddfdMdMdddfddg3dKh1ewi4i5i6i7aadmacaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aai8i9i!i#i$i%i&i*i+i-i;i<i=hDi>i?i@i^i_fYi|iLiLi~g4jajbjcjdhajejfjgjhjijjjkjljmjnjojpjqjrg_jsiYe;e<dddfddg4fZdMdfdMd-jtjujvjwjxaaaajyjzaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aajAjBjCjDjDjEi$jFjGjHjIjJjKjLjMjNjOjPjQjQjRiLiLi|evg_jSjTjUjVjWhKjXjYjZj0haj1j2j3j4j5j6j7iqghj8g2hqe<e<e=dJj9dMdMg4jgj!j#j$j%j&j*j+aaaaagaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"j-j;j<j=j>j?j@j^j_j|j~kakbkckdkekfkgkhkii|i|g^kjfYjakkklkmknkoiNkpeZkqkrhaksktkukvkwkxkykzkAkBkCkDkEiYkFeukGkHkIdMdKdKgpkJeWkKkLjvkMkNkOaabIadaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"e2aakPkQkRkSkTkUkVkWkXkYkZk0k1k2k3k4k5iNk6k7k8k9eui~g4k!k#k$k%k&k*hKk+k-k;k<k=k>k?iVk@k^k_jUk|k~lalblcldevfaiLevdfdfdfdfjgeWlekJewlflglhliaaaag9aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aae2aaljlklllmlnlolplqlrlsltluaClvaalwlxlylzlAjRd+iLi~lBlClDdMlElFlGk~lHlIlJlKlLhaimlMlNlOlPlQlRlSjVlTlUipevdKgNgMevdfdfdKeWeWgpeWlVlWlXlYlZl0aaaddmaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaal1aaaal2l3l4l5l6l7j^l8l9l!arl#g9aaaal$l%l&l*l+d+i~l-l;jRjRfZd-l<l=l>l?l@l^l_k;l|hVk>l_f6l~mambmckwmdmeeWidmfmggWeudddfdfdKeWeWgpeTmhmimjmkmlabaabIadaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaammmnaamompmqmrmsmtmumvmwaamxaablccaamymzmAlDmBi|i|mCiLh^fYesmDmEmFmGmHk-mImJjqmKmLmMmNf#eSfDmOgTmPmQmRmSmTmUmVmWeugMevkGd-eWmXmYjgd-mZm0c4m1m2aaaabIaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaam3aaaam4m5m6m7m8m9m!aafiaaaaabbIaaaam#m$m%m&m*d&l-m+m+i~lDiNm-eUm;gdm<m=m>l^f5m?m@m^dKm_m|m~hhmVnanbncndnenfgMngnhi~k^eWledKfGj#g_nifInjnknlnmaabIaeaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaammmnaannnonpnqnrnsaCafabaaaaaaaddmaantnunvnwnxnym+i|h_e~hJnziLnAmEnBkklMk~lQnCnDg4nEnFk;nGnHnInJgTnKmSnLnMeTnNe?jbeue;eueWgpgpeUmYj#nOnPg6nQnRaaaag9aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaanSaaaanTnUnVnWnXaaabaaaaaaaaaabIaaaanYnZnwn0lAi~evn1iLd+g4lDlCeTj!n2n3n4n5n6n7n8l_n9n!n#n$nGhbn%gMgMnEn&n*n+n-n#eViYeudKdKeWk^dKkJkJn;n<n=n>n?aaadblaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaabmnaan@n^n_n|aan~aaaaaaaaaaaaaddmaah6oaoboboci~jRjRg4g4jQhJgMeuidmEodoedfg4ofm~jVogohoiojokolkFc1jXomonooopoqhPormgdffGifledKosjgeWotouovowoxabaaadabaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaal1aaaaoyozaaabaaaaaaaaaaaaaaaablccaaoAoBoCoDoEiLfYg4g4eueueueWoFgWgMn6oGlak-oHmRoIj0m?oJkkoKoLoMggoNoOoPoQoRoSoToUftdMeuhodKk^dKkJoVoWoXoYoZo0aaadaeaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaabmnaaaaaaaaaaaaaaaaaaaaaaaaaaabagaao1o2o3n7d;o4euo5g4eueuevhJe;eveWo6o7jUmDo8o9oSo!o#dKeSo$o%nco&o*nfo+o-o;o<o=o>mWoCo?dLjgjgeWj#o@nio^o_o|o~aaaaabaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaal1abaaaaaaaaaaaaaaaaaaaaaaaaaaagaaaapapbkkeWd;g4eueueueug4eueuhnoLpcpdgWpepfpgphpileeVpjgZl_pkkkeWplpmpnpokkmRppmdpqo=g3dKjgd-g_eTprpsptpupvpwadadaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaddmaapxpypzoFpApBeueueud+eueueuhJkJippCpDkApEjkpFpGpHoSpIpJkJhJfupKpLjVpMpNpOpPpQpRpSpTeudKdKdKo@eTeTeUpUpVpWaajyaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaablccaapXpYgNeUpZh^i~eueueueueuevgMeuddp0p1p2l_m?lPm|oNp3p4d-p5p6p7p8p9lSp!p#p$mJp%o=dKdKdLoVp&eTeZp*p+d=p-aabIaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaabp;aaaap<p=p>p?jPeueueueueueueueuhneUp@p^p_p2p|p~qaqbqcqdhRqepgjkqfqgqhqiqjp3fEdKkJdKosmYmXeTqkqlqmqnqoqpabaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaadmadaaqqqrmgd-qseueuc1i|g4eueug4i~iMgpoLqtquqvqwqxhaqyk;qzpfqAn*qBqCqDfEqEeWeWeWdKeWleqFqGi4qHqIaaqJqKaablaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaabIaaaaqLqMkkoDqNg4d+eueug4dfqOdMeugMm-kkjan#kDqPlaqQqRmKqSokqTe~qUdfd-eUkJfGg_eUqVqWqXqYqZaabIq0q1q2bIabaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaedmaaq3q4q5q6pZe_fZg4g4q7q7fZq8kGg4n1dfdfq9q!q#mekumKq$knq%nEdLj!ifg_d-dKd-eWeYq&q*q+aaq-aaq;q<q=aaadaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag9aaaaq>q?q@q^q_g4fZfZg4g4fZq8q7dJdfdfc3m^hldKe~kGq|j0q~ragWdKeWdKj#jgeWrbrcrdreaarfadafrgrhrirjaaaeaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaebIaaqprkrln7rmrng4eueug4faq7g4ftdfeue<dKe;e|o8hciProrprqeud-d-kJeUjtrrrsrtaajyaadmrurvrwrxryaaaeaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaagaaaarzrApTgprBdMrCeudfeueudfdfdfdfeudfeurDrErFrGoLeXrHrIrIrJrKrLrMrNaarffMrOaarPrQrRaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaadg9aarSrTrUlFfdrng4dfg4g4dfg4dfdfeueudfdfrViNeZdMded-etd-rWrXrYrZaar0aar1r2r3r4r5r6g9abaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaagaaaar7r8r9d-qsc1g4eueueudfdfdfdfftdMh@gMg4dMdKdKd-far!r#r$aar%aar&r*r+r-r;r<r=aag9aaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaadr>aar?r@r^ngr_khg4eueueudfdfdMg4g4dfeuhog3dKdKd-r|czr~aar%aasasbscsdsesfsgshsiaaaddmaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaeccaasjskipeUsleueuo5g4dfdfkGkGg4eueudfdKd-smn=snaasoaaspsqsrssstsusvswsxsyszaaaag9abaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaabIaaaasAsBpToDsCeug4g4g4fafZg4dfdfdKd-r_h3sDsEjyadq2sFc4sGsHsIsJsKsLsMsNsOsPsQaaaasRaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaeBdmaasSsTg1sUsVpBfZg4fafZdfdfdfk^sWsXsYsZdmjyrus0s1s2s3s4s5s6s7s8aaaas9s!s#s$s%aabIadaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaabIaaaas&s*e|eTsVfZfZfZdfdfdMs+s-s;s<ads=aas>s?s@s^s_s|s~tatbaatcaatdtetftgthtiaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaacagaaabtjtkoFkhsVg4dfdLoDtltmtnaatoaatptqtrtstttutvtwtxtytzaatAtBtCtDtEtFtGtHtIad",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaasRccaatJtKtLf0tMtNoDtOrXtPaatcaatQtRtStTtUtVtWtXtYtZt0t1t2t3t4t5t6t6t7t8t9t!t#",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag9aaaat$t%n#t&t*t+t-aas=aat;t<t=t>t?t@t^t_t|t~uaubucudueufuguhuiujukulumun",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaddmaaaauoupuquraasoabusutuuuvuwuxuyuzuAuBjnuCuDuEuFuGuHufuIuJuKuLuMaabl",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaabladaauNuOaadmadaduPuQpVuRuSuTuUuVuWuXuYuZu0u1u2u3tGu4u5u6u7u8aajyaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaabIaaaablaaaaadaau9u!u#uGu$u%u&dmaau*u+u-u;u<u=u>u?u@u7u^aajyabaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaadadaaaaaaaabIaaaau_u|t9u~aaaaabvavbvcvdvevfvgvhvivjvkbIadaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaadmdmaavlvmvnvovpvqvrvsvtvuvvvwuIvxvyvzadacaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaabIaaaavAvBvCvDvEvFvGvHvIvJvKvLvMaadmaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaebIaai7vNvOvPvQvRvSvTvUvVvWaabIaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaabIaaaavXvYpuvZsuv0v1v2aabIaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaadbIaav3v4v5v6v7v8aajyaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaabIaaaav9v!v#aajyaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaadbIaamyaadmaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaablaaadaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaadaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
};
static const char* sun32_xpm[] = {
"32 32 3 2",
"aa c None",
"ab c #f5c518",
"ac c #c8a000",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaabaaaaaaaaaaaaaaaaababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaabababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaabaaaaaaaaaaaa",
"aaaaaaaaaaabababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaabababaaaaaaaaaa",
"aaaaaaaaaaaaabaaaaaaaaaaaaaaacacacacacaaaaaaaaabababaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaacacabababababacacaaaaaaabaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaacacabababababababababacacaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaacabababababababababababababacaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaacabababababababababababababacaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaacabababababababababababababababacaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaacabababababababababababababababacaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaacabababababababababababababababababacaaaaaaaaaaaa",
"aaababababaaaaacabababababababababababababababababacaaaaaaaaaaaa",
"aaababababaaaaacabababababababababababababababababacaaaaabababab",
"aaaaaaaaaaaaaaacabababababababababababababababababacaaaaabababab",
"aaaaaaaaaaaaaaacabababababababababababababababababacaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaacabababababababababababababababacaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaacabababababababababababababababacaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaacabababababababababababababacaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaacabababababababababababababacaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaabaaaaacacabababababababababacacaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaabababaaaaaaacacabababababacacaaaaaaaaabaaaaaaaaaaaa",
"aaaaaaaaaaabababaaaaaaaaaaaaacacacacacaaaaaaaaaaabababaaaaaaaaaa",
"aaaaaaaaaaaaabaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaabababaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaabaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaababaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaababaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaababaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaababaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
};
static const char* moon32_xpm[] = {
"32 32 2 2",
"aa c None",
"ab c #55648c",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaabababababababaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaababababababababaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaabababababababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaababababababababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaabababababababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaabababababababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaabababababababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaabababababababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaabababababababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaabababababababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaabababababababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaabababababababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaabababababababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaabababababababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaabababababababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaabababababababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaabababababababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaabababababababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaabababababababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaababababababababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaabababababababaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaababababababababaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaabababababababaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
};

// Interpolación lineal entre dos colores (para el degradado por bandas).
static Fl_Color lerp_color(Fl_Color a, Fl_Color b, float t) {
    uchar ar, ag, ab, br, bg, bb;
    Fl::get_color(a, ar, ag, ab);
    Fl::get_color(b, br, bg, bb);
    return fl_rgb_color((uchar)(ar + (br - ar) * t),
                        (uchar)(ag + (bg - ag) * t),
                        (uchar)(ab + (bb - ab) * t));
}

// Bandas de degradado para la cara de un botón (x, y, w, h). Se crean
// ANTES del botón para quedar detrás; el botón lleva FL_NO_BOX y solo
// dibuja su etiqueta sobre las bandas.
static std::vector<Fl_Box*> grad_strips_for(int x, int y, int w, int h) {
    std::vector<Fl_Box*> v;
    for (int yy = y; yy < y + h; yy += 2) {
        int hh = (yy + 2 <= y + h) ? 2 : (y + h - yy);
        Fl_Box* s = new Fl_Box(x, yy, w, hh);
        s->box(FL_FLAT_BOX);
        v.push_back(s);
    }
    return v;
}

// Botón con cara en degradado: bandas + colores extremos por tema.
// (Para los botones de acción los dos temas usan el mismo degradado.)
struct GradBtn {
    std::vector<Fl_Box*> strips;
    Fl_Color top_dark, bot_dark, top_light, bot_light;
};

// Bisel explícito de alto contraste alrededor de un widget (2 px por
// fuera de su rectángulo). raised: luz arriba/izquierda, sombra
// abajo/derecha; hundido: al revés.
struct Bevel {
    Fl_Box *t, *l, *b, *r;
    bool raised;
};

// ---------------- aplicación ----------------
class PendrabootApp {
public:
    Fl_Window* win;
    Fl_Hold_Browser* browser;
    Fl_Box* sel_info;
    Fl_Box* subtitle_box;
    Fl_Box* d1_box;
    Fl_Box* d2_box;
    std::vector<Fl_Box*> grad_strips;  // bandas del degradado del título
    Fl_Box* goldline;
    Fl_Pixmap* sun_px = NULL;     // botón de tema en modo oscuro
    Fl_Pixmap* moon_px = NULL;    // botón de tema en modo claro
    Fl_Pixmap* header_px = NULL;  // icono de la cabecera (XPM 36x36)
    Fl_Box* header_icon_box = NULL;
    std::vector<GradBtn> gradbtns;     // degradados de las caras de botones
    std::vector<Bevel> bevels;         // biseles explícitos de alto contraste
    std::vector<Fl_Box*> frames;   // marcos con relieve de cada sección
    std::vector<Fl_Box*> shadows;  // sombras desplazadas de cada marco
    Fl_Choice* fmt_choice;
    Fl_Input* label_input;
    Fl_Button* fmt_btn;
    Fl_Round_Button* radio_dl;
    Fl_Round_Button* radio_local;
    Fl_Round_Button* radio_sys;
    Fl_Box* d3_box;
    Fl_Button* sys_btn;
    Fl_Choice* ver_choice;
    Fl_Button* dl_btn;
    Fl_Input* iso_input;
    Fl_Button* browse_btn;
    Fl_Button* create_btn;
    Fl_Button* refresh_btn;
    Fl_Button* theme_btn;
    Fl_Button* info_btn;
    // Ventana de ayuda (estilo FLKicker)
    Fl_Window* help_win = NULL;
    Fl_Box* help_title;
    Fl_Box* help_ver;
    Fl_Box* help_motto;
    Fl_Tabs* help_tabs;
    Fl_Group* help_tabgrp[3];
    Fl_Box* help_head[3];
    Fl_Text_Display* help_txt[3];
    Fl_Text_Buffer* help_buf[3];
    Fl_Button* help_close;
    Fl_Pixmap* help_px = NULL;
    Fl_Text_Display* log_disp;
    Fl_Text_Buffer* log_buf;
    Fl_Progress* progress;
    Fl_Box* status;

    std::vector<UsbDev> devs;
    int selected = -1;  // índice en devs
    bool dark = true;
    AsyncRunner runner;
    bool busy = false;
    int pending_action = 0; // 0 nada, 1 dd tras descarga
    std::string pending_iso;
    bool confirmed = false; // la confirmación destructiva ya se pidió
    std::string dl_total_str;
    std::string write_source;      // ISO que realmente se graba (tras isohybrid)
    bool write_source_is_temp = false;
    bool dd_active = false;

    PendrabootApp();
    void add_frame(int x, int y, int w, int h);
    void add_bevel(Fl_Widget* w, bool raised);
    void rescan();
    void update_selection();
    void log(const std::string& s);
    void set_status(const std::string& s);
    void set_busy(bool b);
    void apply_theme();
    void show_help();
    void run_command(const std::string& cmd, int action);
    void on_runner_done(int rc);

    // acciones
    void do_format();
    void do_download_and_create();
    void do_create_local();
    void do_system_iso();
    bool prepare_write_source(const std::string& iso, bool allow_modify);
    void do_dd(const std::string& iso, bool allow_modify);
    void do_download();

    static void cb_browser(Fl_Widget*, void* d) { ((PendrabootApp*)d)->update_selection(); }
    static void cb_refresh(Fl_Widget*, void* d) { ((PendrabootApp*)d)->rescan(); }
    static void cb_theme(Fl_Widget*, void* d) { PendrabootApp* a = (PendrabootApp*)d; a->dark = !a->dark; a->apply_theme(); }
    static void cb_info(Fl_Widget*, void* d) { ((PendrabootApp*)d)->show_help(); }
    static void cb_help_close(Fl_Widget*, void* d) { PendrabootApp* a = (PendrabootApp*)d; if (a->help_win) a->help_win->hide(); }
    static void cb_format(Fl_Widget*, void* d) { ((PendrabootApp*)d)->do_format(); }
    static void cb_download(Fl_Widget*, void* d) { ((PendrabootApp*)d)->do_download_and_create(); }
    static void cb_create(Fl_Widget*, void* d) { ((PendrabootApp*)d)->do_create_local(); }
    static void cb_system(Fl_Widget*, void* d) { ((PendrabootApp*)d)->do_system_iso(); }
    static void cb_browse(Fl_Widget*, void* d) {
        PendrabootApp* a = (PendrabootApp*)d;
        Fl_File_Chooser fc(".", "*.iso", Fl_File_Chooser::SINGLE, "Elegir ISO");
        fc.show();
        while (fc.shown()) Fl::wait();
        if (fc.value()) a->iso_input->value(fc.value());
    }
    static void cb_rescan_timer(void* d) {
        PendrabootApp* a = (PendrabootApp*)d;
        if (!a->busy) a->rescan();
        Fl::repeat_timeout(3.0, cb_rescan_timer, d);
    }
    static void cb_poll_timer(void* d) {
        PendrabootApp* a = (PendrabootApp*)d;
        std::string out;
        int rc = a->runner.poll(out);
        if (!out.empty()) {
            // dd status=progress usa \r: convierte a líneas para el registro
            for (size_t i = 0; i < out.size(); i++) if (out[i] == '\r') out[i] = '\n';
            a->log_buf->append(out.c_str());
            a->log_disp->scroll(1000000, 0);
            // tamaño de la ISO del sistema (la genera el propio script):
            // "ISOSIZE: <n>" fija el total para el progreso del dd
            size_t q = out.find("ISOSIZE:");
            if (q != std::string::npos) {
                unsigned long long sz = strtoull(out.c_str() + q + 8, NULL, 10);
                if (sz > 0) a->runner.dd_total = sz;
            }
            // progreso dd: busca "N bytes" en la salida
            if (a->runner.dd_total > 0) {
                size_t p = out.rfind(" bytes");
                if (p != std::string::npos) {
                    size_t e = p;
                    size_t s = e;
                    while (s > 0 && isdigit((unsigned char)out[s-1])) s--;
                    if (s < e) {
                        unsigned long long done = strtoull(out.substr(s, e - s).c_str(), NULL, 10);
                        float pct = (float)(done * 100.0 / a->runner.dd_total);
                        if (pct > 100) pct = 100;
                        a->progress->value(pct);
                    }
                }
            }
        }
        if (a->runner.progress_total > 0 && !a->runner.progress_file.empty()) {
            struct stat st;
            if (stat(a->runner.progress_file.c_str(), &st) == 0) {
                float pct = (float)(st.st_size * 100.0 / a->runner.progress_total);
                if (pct > 100) pct = 100;
                a->progress->value(pct);
            }
        }
        if (rc == -2) {
            Fl::repeat_timeout(0.2, cb_poll_timer, d);
        } else {
            a->on_runner_done(rc);
        }
    }
};

static PendrabootApp* APP = NULL;

// Marco con relieve + sombra desplazada para una sección.
// Se crea ANTES de los widgets de la sección para quedar detrás.
void PendrabootApp::add_frame(int x, int y, int w, int h) {
    Fl_Box* sh = new Fl_Box(x + 3, y + 3, w, h);
    sh->box(FL_FLAT_BOX);
    shadows.push_back(sh);
    Fl_Box* fr = new Fl_Box(x, y, w, h);
    fr->box(FL_EMBOSSED_BOX);
    frames.push_back(fr);
}

// Bisel explícito de alto contraste alrededor de un widget.
void PendrabootApp::add_bevel(Fl_Widget* w, bool raised) {
    int x = w->x(), y = w->y(), ww = w->w(), hh = w->h();
    Fl_Box* t = new Fl_Box(x - 2, y - 2, ww + 4, 2); t->box(FL_FLAT_BOX);
    Fl_Box* l = new Fl_Box(x - 2, y, 2, hh); l->box(FL_FLAT_BOX);
    Fl_Box* b = new Fl_Box(x - 2, y + hh, ww + 4, 2); b->box(FL_FLAT_BOX);
    Fl_Box* r = new Fl_Box(x + ww, y, 2, hh); r->box(FL_FLAT_BOX);
    bevels.push_back(Bevel{t, l, b, r, raised});
}

PendrabootApp::PendrabootApp() {
    APP = this;
    win = new Fl_Window(980, 700, "Pendraboot");
    win->color(fl_rgb_color(32, 36, 43));

    int x = 18, W = 980 - 36;
    // Barra de título con degradado simulado por bandas: FLTK 1.3 no
    // trae degradados nativos, y subclasificar Fl_Box no enlaza (el
    // FLTK de Tiny Core va sin RTTI). Las bandas se crean antes del
    // título y los botones para quedar detrás.
    for (int i = 0; i < 26; i++) {
        Fl_Box* s = new Fl_Box(0, i * 2, 980, 2);
        s->box(FL_FLAT_BOX);
        grad_strips.push_back(s);
    }
    goldline = new Fl_Box(0, 51, 980, 1);
    goldline->box(FL_FLAT_BOX);
    goldline->color(fl_rgb_color(245, 197, 24));
    // Título
    Fl_Box* title = new Fl_Box(x, 8, 300, 30, "Pendraboot");
    title->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    title->labelsize(24);
    title->labelfont(FL_HELVETICA_BOLD);
    title->labelcolor(fl_rgb_color(245, 197, 24));
    // Icono de la cabecera, pegado al lado de la palabra (XPM 36x36, con
    // transparencia: se ve el degradado detrás, sin cuadro blanco)
    header_px = new Fl_Pixmap(icon36_xpm);
    fl_font(FL_HELVETICA_BOLD, 24);
    {
        int tw = (int)fl_width("Pendraboot");
        header_icon_box = new Fl_Box(x + tw + 8, 6, 36, 36);
        header_icon_box->box(FL_NO_BOX);
        header_icon_box->image(header_px);
    }
    // Iconos del botón de tema (sol = oscuro, luna = claro; XPM 32x32)
    sun_px = new Fl_Pixmap(sun32_xpm);
    moon_px = new Fl_Pixmap(moon32_xpm);
    subtitle_box = new Fl_Box(x, 36, 500, 16, "Formatea y crea USB booteables en FLinux");
    subtitle_box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    subtitle_box->labelsize(11);
    subtitle_box->labelcolor(fl_rgb_color(154, 163, 178));
    // Botón de info (izquierda del botón de tema, arriba a la derecha)
    std::vector<Fl_Box*> s_info = grad_strips_for(980 - 18 - 110 - 10 - 40 - 10 - 32, 12, 32, 32);
    info_btn = new Fl_Button(980 - 18 - 110 - 10 - 40 - 10 - 32, 12, 32, 32, "i");
    info_btn->box(FL_NO_BOX);
    info_btn->labelfont(FL_HELVETICA_BOLD);
    info_btn->labelsize(18);
    gradbtns.push_back(GradBtn{s_info, fl_rgb_color(105, 112, 128), fl_rgb_color(42, 47, 56), fl_rgb_color(250, 250, 250), fl_rgb_color(225, 225, 225)});
    info_btn->tooltip("Ayuda de PendraBoot");
    info_btn->callback(cb_info, this);
    std::vector<Fl_Box*> s_theme = grad_strips_for(980 - 18 - 110 - 10 - 40, 12, 40, 32);
    theme_btn = new Fl_Button(980 - 18 - 110 - 10 - 40, 12, 40, 32, "");
    theme_btn->box(FL_NO_BOX);
    gradbtns.push_back(GradBtn{s_theme, fl_rgb_color(105, 112, 128), fl_rgb_color(42, 47, 56), fl_rgb_color(250, 250, 250), fl_rgb_color(225, 225, 225)});
    theme_btn->callback(cb_theme, this);
    std::vector<Fl_Box*> s_ref = grad_strips_for(980 - 18 - 110, 12, 110, 32);
    refresh_btn = new Fl_Button(980 - 18 - 110, 12, 110, 32, "&Actualizar");
    refresh_btn->box(FL_NO_BOX);
    gradbtns.push_back(GradBtn{s_ref, fl_rgb_color(105, 112, 128), fl_rgb_color(42, 47, 56), fl_rgb_color(250, 250, 250), fl_rgb_color(225, 225, 225)});
    refresh_btn->callback(cb_refresh, this);

    // Panel USB
    add_frame(x, 52, W, 160);
    Fl_Box* h1 = new Fl_Box(x, 56, W, 20, "Dispositivos USB detectados");
    h1->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    h1->labelsize(15);
    h1->labelfont(FL_HELVETICA_BOLD);
    h1->labelcolor(fl_rgb_color(245, 197, 24));
    browser = new Fl_Hold_Browser(x, 78, W, 108);
    browser->column_widths(new int[5]{90, 280, 120, 200, 0});
    browser->callback(cb_browser, this);
    sel_info = new Fl_Box(x, 192, W, 16, "Selecciona una USB.");
    sel_info->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    sel_info->labelsize(12);
    sel_info->labelcolor(fl_rgb_color(154, 163, 178));

    // Panel formatear
    add_frame(x, 212, W, 84);
    Fl_Box* h2 = new Fl_Box(x, 216, W, 20, "Formatear la USB seleccionada");
    h2->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    h2->labelsize(15);
    h2->labelfont(FL_HELVETICA_BOLD);
    h2->labelcolor(fl_rgb_color(245, 197, 24));
    Fl_Box* l1 = new Fl_Box(x, 242, 70, 26, "Formato");
    l1->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    l1->labelsize(13);
    fmt_choice = new Fl_Choice(x + 78, 240, 150, 30);
    fmt_choice->add("FAT32");
    fmt_choice->add("NTFS");
    fmt_choice->add("ext4");
    fmt_choice->add("btrfs");
    fmt_choice->value(0);
    Fl_Box* l2 = new Fl_Box(x + 246, 242, 70, 26, "Etiqueta");
    l2->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    l2->labelsize(13);
    label_input = new Fl_Input(x + 322, 240, 220, 30, "");
    label_input->value("FLINUX-USB");
    std::vector<Fl_Box*> s_fmt = grad_strips_for(x + 560, 240, 130, 30);
    fmt_btn = new Fl_Button(x + 560, 240, 130, 30, "&Formatear");
    fmt_btn->box(FL_NO_BOX);
    gradbtns.push_back(GradBtn{s_fmt, fl_rgb_color(233, 110, 100), fl_rgb_color(192, 57, 43), fl_rgb_color(233, 110, 100), fl_rgb_color(192, 57, 43)});
    fmt_btn->color(fl_rgb_color(192, 57, 43));
    fmt_btn->labelcolor(FL_WHITE);
    fmt_btn->callback(cb_format, this);
    Fl_Box* warn = new Fl_Box(x, 276, W, 16, "El formateo borra TODO el contenido de la USB. Se pedirá confirmación antes de borrar.");
    warn->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    warn->labelsize(12);
    warn->labelcolor(fl_rgb_color(255, 138, 122));

    // Panel booteable
    add_frame(x, 296, W, 212);
    Fl_Box* h3 = new Fl_Box(x, 300, W, 20, "Crear USB booteable");
    h3->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    h3->labelsize(15);
    h3->labelfont(FL_HELVETICA_BOLD);
    h3->labelcolor(fl_rgb_color(245, 197, 24));
    Fl_Group* opts = new Fl_Group(x, 324, W, 180);
    opts->box(FL_NO_BOX);
    radio_dl = new Fl_Round_Button(x + 6, 330, 260, 26, "Descargar FLinux oficial");
    radio_dl->type(FL_RADIO_BUTTON);
    radio_dl->value(1);
    d1_box = new Fl_Box(x + 272, 332, 300, 20, "desde la página oficial de FLinux");
    d1_box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    d1_box->labelsize(12);
    d1_box->labelcolor(fl_rgb_color(154, 163, 178));
    Fl_Box* l3 = new Fl_Box(x + 30, 362, 70, 26, "Versión");
    l3->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    l3->labelsize(13);
    ver_choice = new Fl_Choice(x + 104, 360, 300, 30);
    ver_choice->add("FLinux 17.2 con wifi (128 MB)");
    ver_choice->add("FLinux 17.2 sin wifi (40 MB)");
    ver_choice->value(0);
    std::vector<Fl_Box*> s_dl = grad_strips_for(x + 422, 360, 190, 30);
    dl_btn = new Fl_Button(x + 422, 360, 190, 30, "&Descargar y crear");
    dl_btn->box(FL_NO_BOX);
    gradbtns.push_back(GradBtn{s_dl, fl_rgb_color(255, 232, 130), fl_rgb_color(245, 197, 24), fl_rgb_color(255, 232, 130), fl_rgb_color(245, 197, 24)});
    dl_btn->color(fl_rgb_color(245, 197, 24));
    dl_btn->labelcolor(fl_rgb_color(23, 26, 32));
    dl_btn->callback(cb_download, this);
    radio_local = new Fl_Round_Button(x + 6, 402, 200, 26, "Usar una ISO local");
    radio_local->type(FL_RADIO_BUTTON);
    d2_box = new Fl_Box(x + 212, 404, 340, 20, "un archivo .iso que ya tengas en el equipo");
    d2_box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    d2_box->labelsize(12);
    d2_box->labelcolor(fl_rgb_color(154, 163, 178));
    iso_input = new Fl_Input(x + 30, 432, 400, 30, "");
    std::vector<Fl_Box*> s_br = grad_strips_for(x + 442, 432, 120, 30);
    browse_btn = new Fl_Button(x + 442, 432, 120, 30, "E&xaminar…");
    browse_btn->box(FL_NO_BOX);
    gradbtns.push_back(GradBtn{s_br, fl_rgb_color(105, 112, 128), fl_rgb_color(42, 47, 56), fl_rgb_color(250, 250, 250), fl_rgb_color(225, 225, 225)});
    browse_btn->callback(cb_browse, this);
    std::vector<Fl_Box*> s_cr = grad_strips_for(x + 574, 432, 170, 30);
    create_btn = new Fl_Button(x + 574, 432, 170, 30, "&Crear booteable");
    create_btn->box(FL_NO_BOX);
    gradbtns.push_back(GradBtn{s_cr, fl_rgb_color(100, 165, 245), fl_rgb_color(47, 125, 225), fl_rgb_color(100, 165, 245), fl_rgb_color(47, 125, 225)});
    create_btn->color(fl_rgb_color(47, 125, 225));
    create_btn->labelcolor(FL_WHITE);
    create_btn->callback(cb_create, this);
    // Tercera opción: ISO del sistema instalado (remaster en vivo)
    radio_sys = new Fl_Round_Button(x + 6, 470, 320, 26, "Crear ISO de mi sistema instalado");
    radio_sys->type(FL_RADIO_BUTTON);
    d3_box = new Fl_Box(x + 330, 472, 230, 20, "kernel, core, extensiones y estado actual");
    d3_box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    d3_box->labelsize(12);
    d3_box->labelcolor(fl_rgb_color(154, 163, 178));
    std::vector<Fl_Box*> s_sys = grad_strips_for(x + 574, 468, 200, 30);
    sys_btn = new Fl_Button(x + 574, 468, 200, 30, "Crear &ISO del sistema");
    sys_btn->box(FL_NO_BOX);
    gradbtns.push_back(GradBtn{s_sys, fl_rgb_color(110, 200, 130), fl_rgb_color(46, 125, 50), fl_rgb_color(110, 200, 130), fl_rgb_color(46, 125, 50)});
    sys_btn->color(fl_rgb_color(46, 125, 50));
    sys_btn->labelcolor(FL_WHITE);
    sys_btn->callback(cb_system, this);
    opts->end();

    // Registro
    add_frame(x, 508, W, 116);
    Fl_Box* h4 = new Fl_Box(x, 512, W, 20, "Registro");
    h4->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    h4->labelsize(15);
    h4->labelfont(FL_HELVETICA_BOLD);
    h4->labelcolor(fl_rgb_color(245, 197, 24));
    log_buf = new Fl_Text_Buffer();
    log_disp = new Fl_Text_Display(x, 534, W, 80);
    log_disp->buffer(log_buf);
    log_disp->textsize(12);

    // Progreso y estado
    add_frame(x, 624, W, 52);
    progress = new Fl_Progress(x, 630, W, 16);
    progress->minimum(0);
    progress->maximum(100);
    progress->value(0);
    progress->selection_color(fl_rgb_color(245, 197, 24));
    status = new Fl_Box(x, 652, W, 20, "Estado: listo. Selecciona una USB para empezar.");
    status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    status->labelsize(13);
    status->labelcolor(fl_rgb_color(154, 163, 178));
    Fl_Box* foot = new Fl_Box(x, 678, W, 16, "Pendraboot · FLinux 32 bits · Se ejecuta como root");
    foot->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
    foot->labelsize(11);
    foot->labelcolor(fl_rgb_color(107, 114, 128));

    win->end();
    win->size_range(900, 640, 1200, 900);
    browser->take_focus();
    // Relieves: botones con cara en degradado (FL_NO_BOX, las bandas
    // mandan) y bisel explícito; campos y lista hundidos; desplegables
    // elevados. Los biseles se añaden al final, por encima de todo.
    browser->box(FL_DOWN_BOX);
    log_disp->box(FL_DOWN_BOX);
    progress->box(FL_DOWN_BOX);
    label_input->box(FL_DOWN_BOX);
    iso_input->box(FL_DOWN_BOX);
    fmt_choice->box(FL_UP_BOX);
    ver_choice->box(FL_UP_BOX);
    theme_btn->box(FL_NO_BOX);
    refresh_btn->box(FL_NO_BOX);
    browse_btn->box(FL_NO_BOX);
    fmt_btn->box(FL_NO_BOX);
    dl_btn->box(FL_NO_BOX);
    create_btn->box(FL_NO_BOX);
    sys_btn->box(FL_NO_BOX);
    // Los biseles son widgets nuevos: hay que crearlos con la ventana
    // reabierta (tras win->end() quedarían huérfanos y no se dibujarían).
    win->begin();
    for (size_t i = 0; i < frames.size(); i++) add_bevel(frames[i], true);
    add_bevel(browser, false);
    add_bevel(log_disp, false);
    add_bevel(progress, false);
    add_bevel(label_input, false);
    add_bevel(iso_input, false);
    add_bevel(fmt_choice, true);
    add_bevel(ver_choice, true);
    add_bevel(theme_btn, true);
    add_bevel(info_btn, true);
    add_bevel(refresh_btn, true);
    add_bevel(browse_btn, true);
    add_bevel(fmt_btn, true);
    add_bevel(dl_btn, true);
    add_bevel(create_btn, true);
    add_bevel(sys_btn, true);
    win->end();

    // Ventana de ayuda (estilo FLKicker): se crea oculta y se muestra
    // con el botón "i" de la cabecera. Sin subclases: Fl_Tabs + grupos.
    help_win = new Fl_Window(680, 500, "Ayuda de PendraBoot");
    help_win->begin();
    help_px = new Fl_Pixmap(icon96_xpm);
    {
        Fl_Box* logo_box = new Fl_Box(16, 14, 96, 96);
        logo_box->box(FL_NO_BOX);
        logo_box->image(help_px);
    }
    help_title = new Fl_Box(128, 18, 400, 32, "PendraBoot");
    help_title->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    help_title->labelsize(24);
    help_title->labelfont(FL_HELVETICA_BOLD);
    help_title->labelcolor(fl_rgb_color(245, 197, 24));
    help_ver = new Fl_Box(128, 54, 200, 18, "versión 1.0");
    help_ver->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    help_ver->labelsize(12);
    help_motto = new Fl_Box(128, 74, 520, 18, "Formatea y crea USB booteables en FLinux");
    help_motto->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    help_motto->labelsize(12);
    help_tabs = new Fl_Tabs(16, 126, 648, 300);
    static const char* help_tab_names[3] = { "Qué es", "Cómo se usa", "Autores" };
    static const char* help_body[3] = {
        "PendraBoot es un programa para FLinux que formatea memorias USB y crea USB booteables. Detecta solo memorias extraíbles: el disco interno nunca aparece en la lista. Formatea en FAT32, NTFS, ext4 y btrfs, y crea USB booteables de tres formas: descargando la ISO oficial de FLinux (con wifi o sin wifi), usando una ISO local que ya tengas guardada, o creando una ISO de tu sistema instalado con todos sus programas. Todas las ISO se hacen híbridas, para que la USB arranque escrita en crudo con dd, Rufus o Etcher, y también por Ventoy.",
        "1. Conecta la USB y pulsa Actualizar. 2. Selecciónala en la lista. 3. Para formatear: elige el formato y la etiqueta y pulsa Formatear. El formateo borra TODO el contenido y siempre pide confirmación antes de borrar. 4. Para crear una USB booteable: elige una de las tres opciones y pulsa su botón. 5. Espera a que el registro diga que terminó y no desconectes la USB antes. El programa se ejecuta como root porque necesita acceso directo a los dispositivos.",
        "Idea original: Nicolás Longardi, creador de FLinux, que dio la idea en un directo en Kick y con ella inspiró este proyecto. PendraBoot fue desarrollado por Juan Carlos (Juanca) para la comunidad FLinux."
    };
    for (int i = 0; i < 3; i++) {
        help_tabgrp[i] = new Fl_Group(16, 154, 648, 272, help_tab_names[i]);
        help_tabgrp[i]->box(FL_FLAT_BOX);
        help_head[i] = new Fl_Box(16, 164, 648, 24, help_tab_names[i]);
        help_head[i]->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        help_head[i]->labelsize(15);
        help_head[i]->labelfont(FL_HELVETICA_BOLD);
        help_head[i]->labelcolor(fl_rgb_color(245, 197, 24));
        help_buf[i] = new Fl_Text_Buffer();
        help_buf[i]->text(help_body[i]);
        help_txt[i] = new Fl_Text_Display(16, 192, 648, 220);
        help_txt[i]->buffer(help_buf[i]);
        help_txt[i]->box(FL_FLAT_BOX);
        help_txt[i]->textsize(13);
        help_txt[i]->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
        help_tabgrp[i]->end();
    }
    help_tabs->end();
    std::vector<Fl_Box*> s_help_close = grad_strips_for(280, 442, 120, 32);
    help_close = new Fl_Button(280, 442, 120, 32, "&Cerrar");
    help_close->box(FL_NO_BOX);
    gradbtns.push_back(GradBtn{s_help_close, fl_rgb_color(105, 112, 128), fl_rgb_color(42, 47, 56), fl_rgb_color(250, 250, 250), fl_rgb_color(225, 225, 225)});
    help_close->callback(cb_help_close, this);
    add_bevel(help_close, true);
    help_win->end();

    apply_theme();
}

void PendrabootApp::show_help() {
    if (help_win) help_win->show();
}

void PendrabootApp::log(const std::string& s) {
    log_buf->append(s.c_str());
    log_buf->append("\n");
    log_disp->scroll(1000000, 0);
}
void PendrabootApp::set_status(const std::string& s) {
    std::string t = "Estado: " + s;
    status->copy_label(t.c_str());
}
void PendrabootApp::set_busy(bool b) {
    busy = b;
    if (b) { fmt_btn->deactivate(); dl_btn->deactivate(); create_btn->deactivate(); refresh_btn->deactivate(); }
    else { fmt_btn->activate(); dl_btn->activate(); create_btn->activate(); refresh_btn->activate(); }
}

void PendrabootApp::rescan() {
    std::string keep = selected >= 0 && selected < (int)devs.size() ? devs[selected].name : "";
    devs = scan_usb();
    browser->clear();
    browser->add("Dispositivo\tModelo\tTamaño\tFormato");
    for (size_t i = 0; i < devs.size(); i++) {
        std::string line = devs[i].name + "\t" + devs[i].model + "\t" + devs[i].size + "\t" + devs[i].format;
        browser->add(line.c_str());
    }
    selected = -1;
    for (size_t i = 0; i < devs.size(); i++) {
        if (devs[i].name == keep) { selected = (int)i; browser->select((int)i + 2); break; }
    }
    if (selected < 0 && !devs.empty()) { selected = 0; browser->select(2); }
    update_selection();
}

void PendrabootApp::update_selection() {
    int v = browser->value(); // 1 = cabecera
    if (v >= 2 && v - 2 < (int)devs.size()) selected = v - 2;
    else if (devs.empty()) selected = -1;
    if (selected >= 0) {
        UsbDev& dv = devs[selected];
        std::string t = "Seleccionada: /dev/" + dv.name + " — " + dv.model + " " + dv.size +
                        " (" + dv.format + "). Solo se listan USB extraíbles; el disco interno nunca aparece aquí.";
        sel_info->copy_label(t.c_str());
    } else {
        sel_info->copy_label("No hay USB conectadas. Conecta una memoria USB.");
    }
}

void PendrabootApp::apply_theme() {
    // Sin dynamic_cast: el FLTK de Tiny Core se compila sin RTTI y el
    // enlazador no encuentra los typeinfo. Se colorea cada miembro
    // explícitamente (mismo resultado que el recorrido anterior).
    Fl_Color bg, bg2, fg, muted, field, neutral;
    if (dark) {
        bg = fl_rgb_color(32, 36, 43); bg2 = fl_rgb_color(23, 26, 32);
        fg = fl_rgb_color(232, 232, 232); muted = fl_rgb_color(154, 163, 178);
        field = fl_rgb_color(32, 36, 43);
        neutral = fl_rgb_color(42, 47, 56);
        // Fl::background/background2/foreground en FLTK 1.3 toman (r, g, b)
        Fl::background(32, 36, 43);
        Fl::background2(23, 26, 32);
        Fl::foreground(232, 232, 232);
    } else {
        bg = fl_rgb_color(240, 240, 240); bg2 = fl_rgb_color(255, 255, 255);
        fg = fl_rgb_color(30, 30, 30); muted = fl_rgb_color(100, 100, 100);
        field = fl_rgb_color(255, 255, 255);
        neutral = fl_rgb_color(225, 225, 225);
        Fl::background(240, 240, 240);
        Fl::background2(255, 255, 255);
        Fl::foreground(30, 30, 30);
    }
    win->color(bg);
    // Navegador y registro
    browser->color(bg2);
    browser->labelcolor(fg);
    browser->textcolor(fg);
    browser->selection_color(dark ? fl_rgb_color(58, 65, 80) : fl_rgb_color(180, 200, 230));
    log_disp->color(bg2);
    log_disp->labelcolor(fg);
    log_disp->textcolor(fg);
    // Progreso
    progress->color(bg2);
    progress->selection_color(fl_rgb_color(245, 197, 24));
    // Entradas y desplegables
    fmt_choice->color(field); fmt_choice->labelcolor(fg); fmt_choice->textcolor(fg);
    ver_choice->color(field); ver_choice->labelcolor(fg); ver_choice->textcolor(fg);
    label_input->color(field); label_input->labelcolor(fg); label_input->textcolor(fg);
    iso_input->color(field); iso_input->labelcolor(fg); iso_input->textcolor(fg);
    // Botones neutros y radios (fmt/dl/create/sys conservan su color propio)
    theme_btn->color(neutral); theme_btn->labelcolor(fg);
    info_btn->color(neutral); info_btn->labelcolor(fg);
    refresh_btn->color(neutral); refresh_btn->labelcolor(fg);
    browse_btn->color(neutral); browse_btn->labelcolor(fg);
    radio_dl->color(neutral); radio_dl->labelcolor(fg);
    radio_local->color(neutral); radio_local->labelcolor(fg);
    radio_sys->color(neutral); radio_sys->labelcolor(fg);
    d3_box->labelcolor(muted);
    // Textos tenues (subtítulo, selección, descripciones, estado)
    subtitle_box->labelcolor(muted);
    d1_box->labelcolor(muted);
    d2_box->labelcolor(muted);
    sel_info->labelcolor(muted);
    status->labelcolor(muted);
    // Marcos con relieve y sus sombras
    Fl_Color panel = dark ? fl_rgb_color(48, 53, 64) : fl_rgb_color(255, 255, 255);
    Fl_Color shadowc = dark ? fl_rgb_color(0, 0, 0) : fl_rgb_color(190, 190, 190);
    for (size_t i = 0; i < frames.size(); i++) frames[i]->color(panel);
    for (size_t i = 0; i < shadows.size(); i++) shadows[i]->color(shadowc);
    // Degradado de la barra de título (bandas)
    Fl_Color gtop = dark ? fl_rgb_color(58, 64, 78) : fl_rgb_color(255, 255, 255);
    Fl_Color gbot = dark ? fl_rgb_color(32, 36, 43) : fl_rgb_color(222, 222, 222);
    for (size_t i = 0; i < grad_strips.size(); i++) {
        float t = grad_strips.size() > 1 ? (float)i / (grad_strips.size() - 1) : 0;
        grad_strips[i]->color(lerp_color(gtop, gbot, t));
    }
    // Degradados de las caras de los botones (bandas)
    for (size_t i = 0; i < gradbtns.size(); i++) {
        GradBtn& g = gradbtns[i];
        Fl_Color top = dark ? g.top_dark : g.top_light;
        Fl_Color bot = dark ? g.bot_dark : g.bot_light;
        for (size_t j = 0; j < g.strips.size(); j++) {
            float t = g.strips.size() > 1 ? (float)j / (g.strips.size() - 1) : 0;
            g.strips[j]->color(lerp_color(top, bot, t));
        }
    }
    // Biseles explícitos de alto contraste (luz/sombra mucho más marcada
    // que el UP_BOX estándar, sobre todo en modo oscuro)
    Fl_Color lite = dark ? fl_rgb_color(235, 240, 250) : fl_rgb_color(255, 255, 255);
    Fl_Color darkc = dark ? fl_rgb_color(0, 0, 0) : fl_rgb_color(130, 130, 130);
    for (size_t i = 0; i < bevels.size(); i++) {
        Bevel& b = bevels[i];
        Fl_Color hl = b.raised ? lite : darkc;
        Fl_Color sh = b.raised ? darkc : lite;
        b.t->color(hl); b.l->color(hl); b.b->color(sh); b.r->color(sh);
    }
    // Título y secciones (dorado), aviso (rojo) y pie (gris) se conservan.
    // Botón de tema solo icono: sol en oscuro (pasa a claro), luna
    // cuarto menguante en claro (pasa a oscuro). Sin texto.
    theme_btn->image(dark ? sun_px : moon_px);
    theme_btn->tooltip(dark ? "Cambiar a modo claro" : "Cambiar a modo oscuro");
    // Ventana de ayuda: mismo tema que la principal
    if (help_win) {
        help_win->color(bg);
        help_title->labelcolor(fl_rgb_color(245, 197, 24));
        help_ver->labelcolor(muted);
        help_motto->labelcolor(muted);
        help_tabs->color(bg2);
        for (int i = 0; i < 3; i++) {
            help_tabgrp[i]->labelcolor(fg);
            help_tabgrp[i]->color(bg2);
            help_head[i]->labelcolor(fl_rgb_color(245, 197, 24));
            help_txt[i]->color(bg2);
            help_txt[i]->textcolor(fg);
        }
        help_close->color(neutral);
        help_close->labelcolor(fg);
        help_win->redraw();
    }
    win->redraw();
}

void PendrabootApp::run_command(const std::string& cmd, int action) {
    pending_action = action;
    progress->value(0);
    runner.progress_total = 0;
    runner.progress_file.clear();
    runner.dd_total = 0;
    if (!runner.start(cmd)) {
        log("Error: no se pudo lanzar el comando.");
        set_status("Error al lanzar el comando.");
        set_busy(false);
        return;
    }
    set_busy(true);
    Fl::add_timeout(0.2, cb_poll_timer, this);
}

void PendrabootApp::on_runner_done(int rc) {
    set_busy(false);
    if (rc == 0) progress->value(100);
    if (pending_action == 1) {
        // Terminó la descarga: validar y pasar a dd (la ISO descargada es
        // copia fresca: isohybrid puede modificarla directamente)
        pending_action = 0;
        struct stat st;
        if (rc == 0 && stat(pending_iso.c_str(), &st) == 0 && st.st_size > 1000000) {
            char buf[128];
            snprintf(buf, sizeof buf, "Descarga completa: %s.", human_size(st.st_size).c_str());
            log(buf);
            do_dd(pending_iso, true);
        } else {
            log("Error: la descarga falló o el archivo es demasiado pequeño; no se escribe nada en la USB.");
            set_status("Error en la descarga.");
            progress->value(0);
        }
        return;
    }
    if (dd_active) {
        // Terminó un dd: limpiar la copia temporal si se usó
        dd_active = false;
        if (write_source_is_temp) { unlink(write_source.c_str()); write_source_is_temp = false; }
        write_source.clear();
    }
    if (rc == 0) {
        set_status("Operación completada.");
        rescan();
    } else {
        set_status("La operación terminó con error (ver registro).");
    }
}

static std::string sanitize_label(const std::string& in, const std::string& fs) {
    std::string out;
    for (char c : in) {
        if (c == ' ') c = '_';
        if (isalnum((unsigned char)c) || c == '_' || c == '-') out += c;
    }
    size_t maxlen = 11;
    if (fs == "ext4") maxlen = 16;
    else if (fs == "NTFS") maxlen = 32;
    else if (fs == "btrfs") maxlen = 255;
    if (out.size() > maxlen) out = out.substr(0, maxlen);
    if (fs == "FAT32") for (char& c : out) c = toupper((unsigned char)c);
    if (out.empty()) out = "FLINUX-USB";
    return out;
}

void PendrabootApp::do_format() {
    if (selected < 0) { fl_alert("Selecciona primero una USB."); return; }
    UsbDev dv = devs[selected];
    const char* fmts[] = {"FAT32", "NTFS", "ext4", "btrfs"};
    std::string fs = fmts[fmt_choice->value()];
    std::string label = sanitize_label(label_input->value() ? label_input->value() : "", fs);
    // Destino: primera partición si existe, si no el dispositivo entero
    std::vector<std::string> parts = partitions_of(dv.name);
    std::string target = parts.empty() ? ("/dev/" + dv.name) : parts[0];
    char msg[512];
    snprintf(msg, sizeof msg,
        "Formatear /dev/%s (%s) como %s con etiqueta \"%s\".\n"
        "¡Se borrará TODO el contenido de la USB!",
        dv.name.c_str(), dv.size.c_str(), fs.c_str(), label.c_str());
    int ans = fl_choice("%s", "Cancelar", "Formatear", NULL, msg);
    if (ans != 1) { set_status("Formateo cancelado."); return; }
    // Desmontar particiones antes de formatear
    std::string cmd = "for p in /dev/" + dv.name + "*; do umount \"$p\" 2>/dev/null; done; ";
    if (fs == "FAT32") {
        // -I solo si se formatea el dispositivo entero (sin particiones):
        // mkfs.vfat se niega a tocar un disco entero sin -I.
        cmd += "mkfs.vfat";
        if (parts.empty()) cmd += " -I";
        cmd += " -F 32 -n " + shell_quote(label) + " " + target;
    }
    else if (fs == "NTFS") cmd += "mkfs.ntfs -f -L " + shell_quote(label) + " " + target;
    else if (fs == "ext4") {
        cmd += "mkfs.ext4";
        if (parts.empty()) cmd += " -F";
        cmd += " -L " + shell_quote(label) + " " + target;
    } else cmd += "mkfs.btrfs -f -L " + shell_quote(label) + " " + target;
    cmd += " 2>&1";
    log("$ " + cmd);
    set_status("Formateando /dev/" + dv.name + " como " + fs + "…");
    run_command(cmd, 0);
}

void PendrabootApp::do_download() {
    const char* url = ver_choice->value() == 0 ? URL_WIFI : URL_NOWIFI;
    pending_iso = "/tmp/pendraboot-descarga.iso";
    ::unlink(pending_iso.c_str());
    // Tamaño total para la barra (HEAD, último Content-Length tras redirecciones)
    std::string headcmd = "curl -sIL \"" + std::string(url) + "\" | grep -i '^Content-Length:' | tail -1";
    FILE* p = popen(headcmd.c_str(), "r");
    unsigned long long total = 0;
    if (p) {
        char buf[256];
        if (fgets(buf, sizeof buf, p)) {
            const char* colon = strchr(buf, ':');
            if (colon) total = strtoull(colon + 1, NULL, 10);
        }
        pclose(p);
    }
    std::string cmd = "curl -f -L -o " + shell_quote(pending_iso) + " \"" + url + "\" 2>&1";
    log("$ " + cmd);
    set_status("Descargando FLinux oficial…");
    run_command(cmd, 1);
    runner.progress_total = total;
    runner.progress_file = pending_iso;
}

void PendrabootApp::do_download_and_create() {
    if (selected < 0) { fl_alert("Selecciona primero una USB."); return; }
    if (!radio_dl->value()) radio_dl->value(1);
    UsbDev dv = devs[selected];
    const char* ver = ver_choice->value() == 0 ? "FLinux 17.2 con wifi" : "FLinux 17.2 sin wifi";
    char msg[512];
    snprintf(msg, sizeof msg,
        "Descargar %s y crear una USB booteable en /dev/%s (%s).\n"
        "¡Se borrará TODO el contenido de la USB!",
        ver, dv.name.c_str(), dv.size.c_str());
    int ans = fl_choice("%s", "Cancelar", "Descargar y crear", NULL, msg);
    if (ans != 1) { set_status("Operación cancelada."); return; }
    confirmed = true;
    do_download();
}

void PendrabootApp::do_create_local() {
    if (selected < 0) { fl_alert("Selecciona primero una USB."); return; }
    if (!radio_local->value()) radio_local->value(1);
    std::string iso = iso_input->value() ? iso_input->value() : "";
    if (iso.empty()) { fl_alert("Elige primero una ISO local (Examinar…)."); return; }
    struct stat st;
    if (stat(iso.c_str(), &st) != 0) { fl_alert("La ISO local no existe."); return; }
    do_dd(iso, false);
}

// ---------------- ISO del sistema instalado (tercera opción) ----------------
// Genera una ISO booteable del Tiny Core en ejecución (kernel, core,
// extensiones de tce/optional + onboot.lst, estado persistente vía
// mydata.tgz nativo) y la graba en la USB. TODO se prepara en /tmp
// ANTES de escribir: aunque el tce viva en la USB destino, la copia ya
// está hecha cuando empieza el dd (nunca se lee y escribe el mismo
// dispositivo a la vez). Las extensiones van en cde/ (arranque CD) y en
// tce/ (arranque USB, que busca tce y no cde).
void PendrabootApp::do_system_iso() {
    if (selected < 0) { fl_alert("Selecciona primero una USB."); return; }
    if (!radio_sys->value()) radio_sys->value(1);
    UsbDev dv = devs[selected];
    char msg[512];
    snprintf(msg, sizeof msg,
        "Crear una ISO del sistema en ejecución y grabarla en /dev/%s (%s).\n"
        "Se generará la ISO (kernel, core, extensiones y estado actual),\n"
        "se hará híbrida y se grabará. ¡Se borrará TODO el contenido de la USB!",
        dv.name.c_str(), dv.size.c_str());
    int ans = fl_choice("%s", "Cancelar", "Crear ISO del sistema", NULL, msg);
    if (ans != 1) { set_status("Operación cancelada."); return; }
    static const char* script = R"PENDRA(#!/bin/sh
# Pendraboot: ISO booteable del sistema Tiny Core en ejecución.
set -e
trap 'rm -rf "$STAGE"' EXIT
DEV="$1"
DEST="/dev/$DEV"
STAGE="/tmp/pendraboot-sistema"
ISO="/tmp/pendraboot-sistema.iso"
echo "== Pendraboot: ISO del sistema en ejecución para /dev/$DEV =="
if [ -b /dev/sr0 ] && [ ! -d /mnt/sr0/cde ]; then
  mkdir -p /mnt/sr0
  mount /dev/sr0 /mnt/sr0 2>/dev/null || true
fi
TCEDIR=$(cat /etc/sysconfig/tcedir 2>/dev/null || echo /tmp/tce)
echo "tce del sistema: $TCEDIR"
case "$TCEDIR" in
  /mnt/$DEV/*) echo "NOTA: el tce está en la USB destino; todo se copia a /tmp antes de grabar." ;;
esac
BOOTDIR=""
for d in "$TCEDIR/../boot" /mnt/sr0/boot; do
  if [ -f "$d/vmlinuz" ] && [ -f "$d/core.gz" ]; then BOOTDIR="$d"; break; fi
done
if [ -z "$BOOTDIR" ]; then
  for m in /mnt/*/boot; do
    if [ -f "$m/vmlinuz" ] && [ -f "$m/core.gz" ]; then BOOTDIR="$m"; break; fi
  done
fi
if [ -z "$BOOTDIR" ]; then echo "Error: no se encontró el kernel ni el core del sistema (vmlinuz + core.gz)."; exit 1; fi
echo "arranque desde: $BOOTDIR"
ISOLINUX=""
for f in /usr/local/share/syslinux/isolinux.bin /usr/share/syslinux/isolinux.bin; do
  if [ -f "$f" ]; then ISOLINUX="$f"; break; fi
done
if [ -z "$ISOLINUX" ]; then ISOLINUX=$(find /usr/local /usr/share -name isolinux.bin 2>/dev/null | head -1); fi
if [ -z "$ISOLINUX" ]; then echo "Error: no se encontró isolinux.bin (falta syslinux.tcz)."; exit 1; fi
MKISO=""
if command -v mkisofs >/dev/null 2>&1; then MKISO="mkisofs";
elif command -v genisoimage >/dev/null 2>&1; then MKISO="genisoimage";
elif command -v xorriso >/dev/null 2>&1; then MKISO="xorriso -as mkisofs";
fi
if [ -z "$MKISO" ]; then echo "Error: no hay herramienta para crear ISO (falta cdrtools.tcz)."; exit 1; fi
echo "Guardando el estado actual (filetool.sh -b)..."
filetool.sh -b 2>&1 || echo "Aviso: filetool.sh -b falló; se continúa sin mydata.tgz."
SRC_SZ=0
for p in "$BOOTDIR/vmlinuz" "$BOOTDIR/core.gz" "$TCEDIR/optional" /mnt/sr0/cde/optional "$TCEDIR/mydata.tgz"; do
  if [ -e "$p" ]; then S=$(du -sb "$p" 2>/dev/null | cut -f1); SRC_SZ=$((SRC_SZ + S)); fi
done
NEED=$((SRC_SZ * 2 + 100000000))
AVAIL=$(df -k /tmp | tail -1 | awk '{print $4 * 1024}')
if [ "$NEED" -gt "$AVAIL" ]; then echo "Error: no hay espacio en /tmp (necesario ~$((NEED / 1000000)) MB, disponible ~$((AVAIL / 1000000)) MB)."; exit 1; fi
rm -rf "$STAGE"; mkdir -p "$STAGE/boot/isolinux" "$STAGE/cde/optional" "$STAGE/tce/optional"
cp "$BOOTDIR/vmlinuz" "$STAGE/boot/vmlinuz"
cp "$BOOTDIR/core.gz" "$STAGE/boot/core.gz"
cp "$ISOLINUX" "$STAGE/boot/isolinux/isolinux.bin"
echo "Copiando extensiones..."
cp "$TCEDIR"/optional/*.tcz "$STAGE/cde/optional/" 2>/dev/null || true
for cd in /mnt/sr0/cde "$BOOTDIR/../cde"; do
  if [ -d "$cd/optional" ]; then cp "$cd"/optional/*.tcz "$STAGE/cde/optional/" 2>/dev/null || true; fi
done
ONBOOT=""
if [ -s "$TCEDIR/onboot.lst" ]; then ONBOOT="$TCEDIR/onboot.lst";
else
  for cd in /mnt/sr0/cde "$BOOTDIR/../cde"; do
    if [ -s "$cd/onboot.lst" ]; then ONBOOT="$cd/onboot.lst"; break; fi
  done
fi
if [ -n "$ONBOOT" ]; then
  cp "$ONBOOT" "$STAGE/cde/onboot.lst"; cp "$ONBOOT" "$STAGE/tce/onboot.lst"
  echo "onboot.lst desde: $ONBOOT"
else
  echo "Aviso: no se encontró onboot.lst; la ISO arrancará sin extensiones onboot."
  touch "$STAGE/cde/onboot.lst" "$STAGE/tce/onboot.lst"
fi
cp "$STAGE"/cde/optional/*.tcz "$STAGE/tce/optional/" 2>/dev/null || true
if [ -f "$TCEDIR/mydata.tgz" ]; then
  cp "$TCEDIR/mydata.tgz" "$STAGE/cde/mydata.tgz"; cp "$TCEDIR/mydata.tgz" "$STAGE/tce/mydata.tgz"
  echo "mydata.tgz incluido."
else
  echo "Aviso: sin mydata.tgz (el estado no se restaurará al arrancar la ISO)."
fi
PARAMS=""
for cfg in "$BOOTDIR/syslinux/syslinux.cfg" "$BOOTDIR/extlinux/extlinux.conf" "$BOOTDIR/isolinux/isolinux.cfg" "$BOOTDIR/syslinux.cfg" "$BOOTDIR/extlinux.conf" "$BOOTDIR/isolinux.cfg"; do
  if [ -f "$cfg" ]; then
    PARAMS=$(grep -i '^[[:space:]]*append' "$cfg" | head -1 | sed 's/^[[:space:]]*append[[:space:]]*//I')
    [ -n "$PARAMS" ] && break
  fi
done
CLEAN=""
for tok in $PARAMS; do
  case "$tok" in
    initrd=*|tce=*|mydata=*|cde) ;;
    *) CLEAN="$CLEAN $tok" ;;
  esac
done
{
  echo "default pendraboot"
  echo "label pendraboot"
  echo "  kernel /boot/vmlinuz"
  echo "  append initrd=/boot/core.gz$CLEAN"
} > "$STAGE/boot/isolinux/isolinux.cfg"
echo "Creando la ISO..."
$MKISO -o "$ISO" -V PENDRABOOT -b boot/isolinux/isolinux.bin -c boot/isolinux/boot.cat -no-emul-boot -boot-load-size 4 -boot-info-table "$STAGE" 2>&1
echo "Aplicando isohybrid..."
if command -v isohybrid >/dev/null 2>&1; then
  isohybrid "$ISO" 2>&1 || { echo "Error: isohybrid falló. No se graba la USB."; exit 1; }
else
  echo "Error: falta isohybrid (syslinux.tcz). No se graba la USB."; exit 1
fi
ISOSZ=$(stat -c %s "$ISO")
echo "ISOSIZE: $ISOSZ"
echo "ISO lista: $ISO ($ISOSZ bytes)."
echo "Grabando en /dev/$DEV (dd bs=4M)..."
for p in /dev/$DEV*; do umount "$p" 2>/dev/null || true; done
dd if="$ISO" of="$DEST" bs=4M status=progress conv=fsync 2>&1; sync
echo "Limpieza temporal..."
rm -f "$ISO"
echo "ISO del sistema grabada en /dev/$DEV."
)PENDRA";
    {
        FILE* f = fopen("/tmp/pendraboot-sistema.sh", "w");
        if (!f) { fl_alert("No se pudo escribir el script temporal."); return; }
        fputs(script, f);
        fclose(f);
    }
    log("$ sh /tmp/pendraboot-sistema.sh " + dv.name);
    set_status("Generando la ISO del sistema en ejecución…");
    run_command("sh /tmp/pendraboot-sistema.sh " + dv.name + " 2>&1", 0);
}

// ---------------- ISO híbrida (isohybrid de syslinux) ----------------
// Una ISO es híbrida si tiene firma MBR 55AA y al menos una entrada de
// la tabla de particiones no nula.
static bool is_hybrid_iso(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char mbr[512];
    size_t n = fread(mbr, 1, sizeof mbr, f);
    fclose(f);
    if (n < 512) return false;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return false;
    for (int i = 446; i < 510; i++) if (mbr[i] != 0) return true;
    return false;
}
static bool run_isohybrid(const std::string& path) {
    std::string cmd = "isohybrid " + shell_quote(path) + " 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    char buf[256];
    while (fgets(buf, sizeof buf, p)) { /* salida ignorada; el registro la resume */ }
    int rc = pclose(p);
    return rc == 0;
}

// Prepara la ISO a grabar: si ya es híbrida se usa tal cual; si no, se
// aplica isohybrid. Con allow_modify=true se modifica el fichero dado
// (vía de descarga: copia fresca); si no, se trabaja sobre una copia
// temporal y el original del usuario nunca se toca.
// Devuelve false si no se pudo preparar (no se graba nada).
bool PendrabootApp::prepare_write_source(const std::string& iso, bool allow_modify) {
    write_source.clear();
    write_source_is_temp = false;
    if (is_hybrid_iso(iso)) {
        log("La ISO ya es híbrida; no se modifica.");
        write_source = iso;
        return true;
    }
    if (system("command -v isohybrid >/dev/null 2>&1") != 0) {
        log("Error: isohybrid no está instalado (falta syslinux.tcz). No se graba la USB.");
        return false;
    }
    if (allow_modify) {
        log("Aplicando isohybrid a la ISO descargada...");
        if (!run_isohybrid(iso)) {
            log("Error: isohybrid falló. No se graba la USB.");
            return false;
        }
        log("ISO híbrida lista.");
        write_source = iso;
        return true;
    }
    // Vía local: copia temporal
    std::string tmp = "/tmp/pendraboot-hibrida.iso";
    log("Copiando la ISO local a un temporal (el original no se modifica)...");
    std::string cmd = "cp " + shell_quote(iso) + " " + shell_quote(tmp) + " 2>&1";
    if (system(cmd.c_str()) != 0) {
        log("Error: no se pudo copiar la ISO a temporal. No se graba la USB.");
        return false;
    }
    log("Aplicando isohybrid a la copia temporal...");
    if (!run_isohybrid(tmp)) {
        log("Error: isohybrid falló. No se graba la USB.");
        unlink(tmp.c_str());
        return false;
    }
    log("ISO híbrida lista.");
    write_source = tmp;
    write_source_is_temp = true;
    return true;
}

void PendrabootApp::do_dd(const std::string& iso, bool allow_modify) {
    if (selected < 0) { fl_alert("Selecciona primero una USB."); return; }
    UsbDev dv = devs[selected];
    struct stat st;
    if (stat(iso.c_str(), &st) != 0) { fl_alert("No se encontró la ISO."); return; }
    if (!confirmed) {
        char msg[512];
        snprintf(msg, sizeof msg,
            "Crear USB booteable en /dev/%s (%s) con la ISO:\n%s\n"
            "¡Se borrará TODO el contenido de la USB!",
            dv.name.c_str(), dv.size.c_str(), iso.c_str());
        int ans = fl_choice("%s", "Cancelar", "Crear booteable", NULL, msg);
        if (ans != 1) { set_status("Operación cancelada."); return; }
    }
    confirmed = false;
    if (!prepare_write_source(iso, allow_modify)) {
        set_busy(false);
        progress->value(0);
        set_status("Error: no se pudo preparar la ISO.");
        return;
    }
    std::string src = write_source;
    if (stat(src.c_str(), &st) != 0) { fl_alert("No se encontró la ISO preparada."); return; }
    std::string cmd = "for p in /dev/" + dv.name + "*; do umount \"$p\" 2>/dev/null; done; ";
    cmd += "dd if=" + shell_quote(src) + " of=/dev/" + dv.name + " bs=4M status=progress conv=fsync 2>&1; sync";
    log("$ dd if=" + src + " of=/dev/" + dv.name + " bs=4M conv=fsync");
    set_status("Escribiendo la ISO en /dev/" + dv.name + "…");
    run_command(cmd, 0);
    runner.dd_total = (unsigned long long)st.st_size;
    dd_active = true;
}

int main(int argc, char** argv) {
    PendrabootApp app;
    app.win->show(argc, argv);
    app.rescan();
    Fl::add_timeout(3.0, PendrabootApp::cb_rescan_timer, &app);
    return Fl::run();
}
