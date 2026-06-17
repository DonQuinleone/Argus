// Optional QA / sign-off metadata attached to an exported report (PDF/JSON).
// Entered in the desktop app's "QA Report Details" window and persisted between
// launches. Every field is individually toggleable; a field is rendered only when
// its toggle is on and its value is non-empty. The report date is generated at
// export time and shown when `showDate` is set.
#pragma once

#include <string>

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
    bool showLogo = true;

    // True if any text field will be rendered (drives whether a QA block is drawn).
    bool anyText() const {
        return (showEngineer && !qaEngineer.empty()) || (showContact && !contact.empty()) ||
               (showStudio && !studio.empty()) || (showClient && !client.empty()) ||
               (showProject && !project.empty()) || (showCatalog && !catalog.empty()) ||
               (showNotes && !notes.empty());
    }
};

}  // namespace argus
