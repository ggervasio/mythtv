function testDBSettings() {
    var result = 0;
    var host = $("#dbHostName").val();
    var user = $("#dbUserName").val();
    var pass = $("#dbPassword").val();
    var name = $("#dbName").val();
    var port = $("#dbPort").val();

    clearEditMessages();

    if (name == null)
        name = "mythconverg";

    if (port == null)
        port = 3306;

    $.post("/Myth/TestDBSettings",
        { HostName: host, UserName: user, Password: pass, DBName: name, dbPort: port},
        function(data) {
            if (data.bool == "true") {
                result = 1;
                setEditStatusMessage("Database connection succeeded!");
            }
            else
                setEditErrorMessage("Database connection failed!");
        }, "json").error(function(data) {
            setEditErrorMessage("Database connection failed!");
        });

    return result;
}

function validateSettingsInDiv(divName) {
    var result = true;
    $("#" + divName + " :input").each(function() {
        if (($(this).attr("type") != "button") &&
            ($(this).attr("type") != "submit") &&
            ($(this).attr("type") != "reset")) {
            if (!validateSetting($(this).attr("id")))
                result = false;
        }
    });

    return result;
}

function saveWizard() {
    if (!validateSettingsInDiv("wizard-network")) {
        setEditErrorMessage("Network Setup has an error.");
    }

    alert("Saving is not fully functional, the database has not been modified!");
}

function preloadWizardTabs() {
    $tabs = $("#wizardtabs").tabs({ cache: true });
    var total = $tabs.find('.ui-tabs-nav li').length;
    var currentLoadingTab = 0;
    $tabs.bind('tabsload',function(){
        currentLoadingTab++;
        if (currentLoadingTab < total)
            $tabs.tabs('load',currentLoadingTab);
        else
            $tabs.unbind('tabsload');
    }).tabs('load',currentLoadingTab);
}

preloadWizardTabs();
$("#editborder").attr({ class: 'editborder-wizard' });
$("#editsavebutton").show();
$("#editsavelink").attr("href", "javascript:saveWizard()");
showEditWindow();

