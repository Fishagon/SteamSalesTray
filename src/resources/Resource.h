#pragma once

// Icons
#define IDI_APPICON            101

// Tray menu command IDs (also reused for the sales/settings windows later).
// Summary rows are disabled display-only items but still need unique IDs.
#define IDM_SUMMARY_TODAY      40001
#define IDM_SUMMARY_YESTERDAY  40005
#define IDM_SUMMARY_7DAY       40002
#define IDM_SUMMARY_30DAY      40003
#define IDM_SUMMARY_LIFETIME   40004

#define IDM_VIEW_SALES         40010
#define IDM_REFRESH_NOW        40011
#define IDM_SETTINGS           40012
#define IDM_OPEN_FINANCIALS    40013
#define IDM_ABOUT              40014
#define IDM_EXIT               40015

// Settings dialog + its controls
#define IDD_SETTINGS           1000
#define IDC_EDIT_APIKEY        1001
#define IDC_STATIC_KEYSTATUS   1002
#define IDC_CHECK_SHOWKEY      1003
#define IDC_BUTTON_TEST        1004
#define IDC_STATIC_TESTRESULT  1005
#define IDC_COMBO_INTERVAL     1006
#define IDC_CHECK_STARTUP      1007
#define IDC_CHECK_NOTIFY       1008
#define IDC_BUTTON_CLEARCACHE  1009
#define IDC_BUTTON_REMOVEKEY   1010
#define IDC_CHECK_HIDELIFETIME 1011
#define IDC_EDIT_PUBLISHER_APIKEY      1012
#define IDC_STATIC_PUBLISHER_KEYSTATUS 1013
#define IDC_BUTTON_REMOVE_PUBLISHERKEY 1014

// Product Sales window controls
#define IDC_SALES_PERIOD       1100
#define IDC_SALES_STATUS       1101
#define IDC_SALES_LIST         1102
#define IDC_SALES_TOTAL        1103
#define IDC_SALES_REFRESH      1104
#define IDC_SALES_SETTINGS     1105
#define IDC_SALES_CLOSE        1106
#define IDC_SALES_WISHLIST     1107
