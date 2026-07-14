#ifndef slic3r_SdCardPanel_hpp_
#define slic3r_SdCardPanel_hpp_

#include <wx/panel.h>
#include <wx/listbox.h>
#include <wx/stattext.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <boost/thread.hpp>

class Button;

namespace Slic3r {

class MachineObject;

namespace GUI {

// Datei-Browser fuer die SD-Karte im LAN-Only-Modus.
// Listet Dateien per FTPS (Port 990) und startet Drucke per MQTT-Befehl.
class SdCardPanel : public wxPanel
{
public:
    SdCardPanel(wxWindow *parent);
    ~SdCardPanel();

    void UpdateByObj(MachineObject *obj);
    void msw_rescale() {}

private:
    void refresh_file_list();
    void on_list_result(std::string const &machine, int curl_result, std::vector<std::string> files);
    void start_print();
    void set_status(wxString const &msg);

    std::string m_machine;
    std::string m_lan_ip;
    std::string m_access_code;
    bool        m_connected = false;

    std::atomic<bool>            m_listing{false};
    std::shared_ptr<std::atomic<bool>> m_alive;
    boost::thread                m_list_thread;

    wxStaticText *m_status_text = nullptr;
    wxListBox    *m_file_list   = nullptr;
    Button       *m_button_refresh = nullptr;
    Button       *m_button_print   = nullptr;
};

}} // namespace Slic3r::GUI

#endif
