#include "SdCardPanel.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "DeviceManager.hpp"
#include "DeviceCore/DevManager.h"
#include "MsgDialog.hpp"
#include "Widgets/Button.hpp"

#include <wx/sizer.h>
#include <curl/curl.h>
#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <algorithm>

namespace Slic3r {
namespace GUI {

static size_t sdcard_write_cb(void *data, size_t size, size_t nmemb, void *userp)
{
    auto *str = static_cast<std::string *>(userp);
    str->append(static_cast<char *>(data), size * nmemb);
    return size * nmemb;
}

static bool is_printable_file(std::string const &name)
{
    if (name.empty() || name.front() == '.') return false;
    std::string lower = boost::algorithm::to_lower_copy(name);
    return boost::algorithm::ends_with(lower, ".3mf") || boost::algorithm::ends_with(lower, ".gcode");
}

SdCardPanel::SdCardPanel(wxWindow *parent)
    : wxPanel(parent, wxID_ANY)
{
    m_alive = std::make_shared<std::atomic<bool>>(true);

    SetBackgroundColour(*wxWHITE);

    auto *main_sizer = new wxBoxSizer(wxVERTICAL);

    m_status_text = new wxStaticText(this, wxID_ANY, _L("Kein Drucker verbunden."));
    main_sizer->Add(m_status_text, 0, wxALL, FromDIP(10));

    m_file_list = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxLB_SINGLE);
    main_sizer->Add(m_file_list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

    auto *btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_button_refresh = new Button(this, _L("Aktualisieren"));
    m_button_print   = new Button(this, _L("Ausgewählte Datei drucken"));
    btn_sizer->Add(m_button_refresh, 0, wxRIGHT, FromDIP(10));
    btn_sizer->Add(m_button_print, 0);
    main_sizer->Add(btn_sizer, 0, wxALL, FromDIP(10));

    SetSizer(main_sizer);

    m_button_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { refresh_file_list(); });
    m_button_print->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { start_print(); });
    m_file_list->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent &) { start_print(); });
}

SdCardPanel::~SdCardPanel()
{
    *m_alive = false;
    if (m_list_thread.joinable())
        m_list_thread.detach();
}

void SdCardPanel::set_status(wxString const &msg)
{
    m_status_text->SetLabelText(msg);
    Layout();
}

void SdCardPanel::UpdateByObj(MachineObject *obj)
{
    if (!obj) {
        m_machine.clear();
        m_lan_ip.clear();
        m_access_code.clear();
        m_connected = false;
        m_file_list->Clear();
        set_status(_L("Kein Drucker verbunden."));
        return;
    }

    bool machine_changed = obj->get_dev_id() != m_machine;
    m_machine     = obj->get_dev_id();
    m_lan_ip      = obj->get_dev_ip();
    m_access_code = obj->get_access_code();
    m_connected   = obj->is_connected();

    if (machine_changed) {
        m_file_list->Clear();
        refresh_file_list();
    }
}

void SdCardPanel::refresh_file_list()
{
    if (m_listing) return;
    if (m_lan_ip.empty() || m_access_code.empty()) {
        set_status(_L("IP-Adresse oder Access Code fehlt. Bitte Drucker verbinden."));
        return;
    }

    m_listing = true;
    set_status(_L("Lese Dateiliste von der SD-Karte..."));

    std::string ip      = m_lan_ip;
    std::string code    = m_access_code;
    std::string machine = m_machine;
    auto        alive   = m_alive;

    if (m_list_thread.joinable())
        m_list_thread.detach();

    m_list_thread = boost::thread([this, ip, code, machine, alive]() {
        std::string listing;
        CURL *curl = curl_easy_init();
        CURLcode res = CURLE_FAILED_INIT;
        if (curl) {
            std::string url = "ftps://" + ip + ":990/";
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_USERPWD, ("bbl:" + code).c_str());
            curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_DIRLISTONLY, 1L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sdcard_write_cb);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &listing);
            res = curl_easy_perform(curl);
            curl_easy_cleanup(curl);
        }

        std::vector<std::string> files;
        if (res == CURLE_OK) {
            std::vector<std::string> lines;
            boost::algorithm::split(lines, listing, boost::algorithm::is_any_of("\r\n"), boost::algorithm::token_compress_on);
            for (auto &line : lines)
                if (is_printable_file(line))
                    files.push_back(line);
            std::sort(files.begin(), files.end());
        } else {
            BOOST_LOG_TRIVIAL(error) << "SdCardPanel: FTPS listing failed, curl code " << res;
        }

        wxGetApp().CallAfter([this, alive, machine, res, files]() {
            if (!*alive) return;
            on_list_result(machine, (int) res, files);
        });
    });
}

void SdCardPanel::on_list_result(std::string const &machine, int curl_result, std::vector<std::string> files)
{
    m_listing = false;
    if (machine != m_machine) return;

    if (curl_result != 0) {
        set_status(_L("Verbindung zur SD-Karte fehlgeschlagen. Bitte IP-Adresse, Access Code und Netzwerk prüfen."));
        return;
    }

    m_file_list->Clear();
    for (auto &f : files)
        m_file_list->Append(wxString::FromUTF8(f.c_str()));

    if (files.empty())
        set_status(_L("Keine druckbaren Dateien (.3mf / .gcode) auf der SD-Karte gefunden."));
    else
        set_status(wxString::Format(_L("%d Dateien auf der SD-Karte. Datei auswählen und drucken (Doppelklick startet ebenfalls)."), (int) files.size()));
}

void SdCardPanel::start_print()
{
    int sel = m_file_list->GetSelection();
    if (sel == wxNOT_FOUND) {
        set_status(_L("Bitte zuerst eine Datei aus der Liste auswählen."));
        return;
    }
    std::string name = m_file_list->GetString(sel).utf8_string();

    DeviceManager *dev = wxGetApp().getDeviceManager();
    MachineObject *obj = dev ? dev->get_selected_machine() : nullptr;
    if (!obj || obj->get_dev_id() != m_machine || !obj->is_connected()) {
        set_status(_L("Drucker ist nicht verbunden. Druck kann nicht gestartet werden."));
        return;
    }

    MessageDialog dlg(this,
        wxString::Format(_L("Soll die Datei \"%s\" von der SD-Karte gedruckt werden?\n\nBitte sicherstellen, dass das Druckbett leer ist."), wxString::FromUTF8(name.c_str())),
        _L("Druck starten"), wxICON_QUESTION | wxYES_NO);
    if (dlg.ShowModal() != wxID_YES) return;

    std::string lower = boost::algorithm::to_lower_copy(name);
    json j;
    if (boost::algorithm::ends_with(lower, ".gcode")) {
        j["print"]["command"]     = "gcode_file";
        j["print"]["param"]       = "/sdcard/" + name;
        j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    } else {
        std::string stem = name.substr(0, name.find_last_of('.'));
        j["print"]["command"]        = "project_file";
        j["print"]["param"]          = "Metadata/plate_1.gcode";
        j["print"]["url"]            = "file:///sdcard/" + name;
        j["print"]["subtask_name"]   = stem;
        j["print"]["project_id"]     = "0";
        j["print"]["profile_id"]     = "0";
        j["print"]["task_id"]        = "0";
        j["print"]["subtask_id"]     = "0";
        j["print"]["bed_type"]       = "auto";
        j["print"]["timelapse"]      = false;
        j["print"]["bed_leveling"]   = true;
        j["print"]["flow_cali"]      = false;
        j["print"]["vibration_cali"] = false;
        j["print"]["layer_inspect"]  = false;
        j["print"]["use_ams"]        = true;
        j["print"]["sequence_id"]    = std::to_string(MachineObject::m_sequence_id++);
    }

    int rtn = obj->publish_json(j, 1);
    if (rtn == 0)
        set_status(wxString::Format(_L("Druckbefehl für \"%s\" wurde an den Drucker gesendet."), wxString::FromUTF8(name.c_str())));
    else
        set_status(_L("Druckbefehl konnte nicht gesendet werden. Bitte Verbindung prüfen."));
}

}} // namespace Slic3r::GUI
