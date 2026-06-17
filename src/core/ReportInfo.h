// Optional QA / sign-off metadata attached to an exported report (PDF/JSON).
// Entered in the desktop app's "QA Report Details" window and persisted between
// launches. Every field is individually toggleable; a field is rendered only when
// its toggle is on and its value is non-empty. The report date is generated at
// export time and shown when `showDate` is set.
#pragma once

#include <string>
#include <vector>

namespace argus {

struct ReportInfo {
    std::string qaEngineer;  // name of the QA engineer
    std::string contact;     // email / phone
    std::string studio;      // studio / company performing the QA
    std::string client;      // who the masters are QA'd for
    std::string project;     // release / album / project title
    std::string catalog;     // catalog number or internal job / ticket ref
    std::string notes;       // free-text sign-off notes

    bool showEngineer = true;
    bool showContact = true;
    bool showStudio = true;
    bool showClient = true;
    bool showProject = true;
    bool showCatalog = true;
    bool showNotes = true;
    bool showDate = true;
    bool showLogo = true;  // deprecated: Argus branding is now always in the footer

    // Include every diagram (per-finding close-ups, goniometer, DC meter) for all findings,
    // not only Warn/Fail ones.
    bool allDiagrams = false;

    // Optional organisation logo, decoded to RGB by the UI. When present it's drawn in the
    // report header (and the Argus logo moves to the footer).
    std::string orgLogoPath;
    std::vector<unsigned char> orgLogoRGB;  // w*h*3
    int orgLogoW = 0, orgLogoH = 0;

    // True if any text field will be rendered (drives whether a QA block is drawn).
    bool anyText() const {
        return (showEngineer && !qaEngineer.empty()) || (showContact && !contact.empty()) ||
               (showStudio && !studio.empty()) || (showClient && !client.empty()) ||
               (showProject && !project.empty()) || (showCatalog && !catalog.empty()) ||
               (showNotes && !notes.empty());
    }
};

}  // namespace argus
