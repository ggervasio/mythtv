var sgTabIDs = new Array();
var sgTabNames = new Array();
var sgTabCount = 0;

function appendTabRow(tabID, id, group, host, dir) {
    var rowNum = $('#sgtable-' + tabID + ' tr').length;
    var altText = "";
    if ((rowNum/2) % 1 == 0)
        altText = "class='alt' ";
    var rowID = "sgtable-" + tabID + "-" + rowNum;
    $("#sgtable-" + tabID + " tr:last").after("<tr " + altText + "id='" + rowID + "'><td class='invisible'>" + id +"</td><td>" + host + "</td><td>" + dir + "</td><td><input type='button' onClick=\"javascript:removeStorageGroupTableRow(" + tabID + ", '" + rowID + "')\" value='Delete'/></tr>");
}

function initStorageGroups() {
    var selectedHost = $("#sgShowHost").val();

    $("#storagegrouptabs").tabs({ cache: true });

    while (sgTabCount > 0) {
        $("#storagegrouptabs").tabs("remove", 0);
        sgTabCount--;
    }
    sgTabIDs = new Array();
    sgTabNames = new Array();
    sgTabCount = 0;

    $.getJSON("/Myth/GetStorageGroupDirs", function(data) {
        var sgTabID;
        var sgHosts = {};
        $.each(data.StorageGroupDirList.StorageGroupDirs, function(i, value) {
            if (sgHosts[value.HostName.toLowerCase()] == undefined) {
                sgHosts[value.HostName.toLowerCase()] = 1;
            }

            if ((selectedHost != "ALL") &&
                (selectedHost.toLowerCase() != value.HostName.toLowerCase())) {
                return true;
            }

            if (value.GroupName in sgTabIDs) {
                sgTabID = sgTabIDs[value.GroupName];
            } else {
                sgTabID = sgTabCount++;
                sgTabIDs[value.GroupName] = sgTabID;
                sgTabNames[sgTabID] = value.GroupName;
                $("#storagegrouptabs").tabs("add", "#sgtabs-" + sgTabID, value.GroupName, sgTabID);
                $("#sgtabs-" + sgTabID).html("<div id='sgtabs-" + sgTabID + "-add'><input type=button value='Add Directory' onClick='javascript:addDir(" + sgTabID + ")'></div><div id='sgtabs-" + sgTabID + "-edit' style='display: none'><b>Adding new Storage Group Directory</b><br><table border=0 cellpadding=2 cellspacing=2><tr><th align=right>Host:</th><td>" + hostsSelect("sgtabs-" + sgTabID + "-edit-hostname") + "</td></tr><tr><th align=right>New Directory:</th><td><input id='sgtabs-" + sgTabID + "-edit-dirname' size=40><input type=button onClick='javascript:browseForNewDir(" + sgTabID + ", \"sgtabs-" + sgTabID + "-edit-dirname\")' value='Browse'><input type=hidden id='sgtabs-" + sgTabID + "-edit-groupname' value='" + value.GroupName + "'></td></tr><tr><td colspan=2><input type=button value='Save' onClick='javascript:saveDir(" + sgTabID + ")'> <input type=button value='Cancel' onClick='javascript:cancelDir(" + sgTabID + ")'></td></tr></table><hr></div><table id='sgtable-" + sgTabID + "' border=1 cellpadding=4 cellspacing=0 width='100%'><th class='invisible'>DirID</th><th>Host Name</th><th>Directory Path</th><th>Actions</th></tr></table><br><input type=button value='Delete Storage Group' onClick='javascript:deleteStorageGroup(" + sgTabID + ")'>");
            }

            appendTabRow(sgTabID, value.Id, value.GroupNme, value.HostName, value.DirName);
        });

        var sortedKeys = new Array();
        for (var i in sgHosts) {
            sortedKeys.push(i);
        }
        sortedKeys.sort();

        var sgHostSelect = "Host: <select id='sgShowHost' "
            + "onChange='javascript:initStorageGroups()'><option value='ALL'>"
            + "Display ALL Hosts</option>;";
        for (var i in sortedKeys) {
            sgHostSelect += "<option";

            if (selectedHost.toLowerCase() == sortedKeys[i])
                sgHostSelect += " selected";

            sgHostSelect += ">" + sortedKeys[i] + "</option>";
        }

        sgHostSelect += "</select>";
        $("#sgHostSelect").html(sgHostSelect);
    });
}

function addDir(tabID) {
    $("#sgtabs-" + tabID + "-add").css("display", "none");
    $("#sgtabs-" + tabID + "-edit").css("display", "");
}

function saveDir(tabID) {
    var group = $("#sgtabs-" + tabID + "-edit-groupname").val();
    var host  = $("#sgtabs-" + tabID + "-edit-hostname").val();
    var dir   = $("#sgtabs-" + tabID + "-edit-dirname").val();

    if (addStorageGroupDir(group, dir, host)) {
        appendTabRow(tabID, 0, group, host, dir);
        $("#sgtabs-" + tabID + "-add").css("display", "");
        $("#sgtabs-" + tabID + "-edit").css("display", "none");
        setHeaderStatusMessage("Storage Group Directory save Succeeded.");
        $("#sgtabs-" + tabID + "-edit-dirname").val("");
    } else {
        setHeaderErrorMessage("Storage Group Directory save Failed!");
    }
}

function cancelDir(tabID) {
    $("#sgtabs-" + tabID + "-add").css("display", "");
    $("#sgtabs-" + tabID + "-edit").css("display", "none");
}

function deleteStorageGroup( tabID ) {
    alert("Are you sure?  Deleting Storage Groups is not hooked up yet anyway "
        + ", but you can delete all directories in a Storage Group "
        + "individually.");
}

function removeStorageGroupTableRow( tabID, rowID ) {
    var id = $("#" + rowID).find("td").eq(0).html();
    var group = sgTabNames[tabID];
    var host = $("#" + rowID).find("td").eq(1).html();
    var dir = $("#" + rowID).find("td").eq(2).html();

    if (removeStorageGroupDir(group, dir, host)) {
        setHeaderStatusMessage("Remove Storage Group Directory Succeeded.");
        $("#" + rowID).remove();
    } else {
        setHeaderErrorMessage("Remove Storage Group Directory Failed!");
    }
}

var sgNewDirInputID;
function newDirSelected(file) {
    $("#" + sgNewDirInputID).val(file);
}

function browseForNewDir( tabID, inputID ) {
    var dirs = new Array;
    sgNewDirInputID = inputID;
    openFileBrowser("Storage Directory", dirs, newDirSelected);
}

//////////////////////////////////////////////////////////////////////////////

initStorageGroups();

