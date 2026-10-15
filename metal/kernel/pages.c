/* Pages shipped inside the kernel image, registered as "patra" objects at boot. */
#include "verum.h"

static const char HOME[] =
"# home\n\n"
"Welcome to the VERUM browser. This page is an object: its address is the hash of its content,\n"
"so it can never change under you, and `why` tells you who made it. Links go to exact content or nowhere.\n\n"
"## Start here\n\n"
"=> page:about what this browser is, and why it is not a web browser\n"
"=> page:sanskrit संस्कृतलिपि - programming in Sanskrit\n"
"=> tag:note your notes\n"
"=> tag:stats results produced by cells\n"
"=> tag:code programs\n\n"
"## Try\n\n"
"* follow 1   - go to the first link\n"
"* back       - return\n"
"* links      - list the links on this page\n"
"* page <title>   - write a page of your own, line by line, ending with a lone .\n\n"
"> नास्ति सत्यात् परो धर्मः - there is no law higher than truth.\n";

static const char ABOUT[] =
"# about\n\n"
"## This is not a web browser\n\n"
"A web page is bytes from an address that can change at any time, running scripts with the\n"
"browser's full authority, leaving a history you cannot prove. A पत्र page is the opposite:\n\n"
"* a page is a content-addressed object: its hash is its address, and it cannot change\n"
"* a link resolves to exact content or not at all\n"
"* a page's script is a cell with exactly the capabilities you grant, listed before it runs\n"
"* your history is the ledger: provable, replayable, undoable\n"
"* there are no cookies or trackers, because a page holds no capability it was not given\n\n"
"## The format\n\n"
"```\n"
"# heading        ## sub-heading\n"
"plain paragraphs, * list items, > quotations\n"
"=> target label  (target: hash, tag:name, page:title)\n"
"@cell code store[get] console[print]\n"
"```\n\n"
"=> page:home back to the start\n";

static const char SANSKRIT[] =
"# sanskrit\n\n"
"## संस्कृतलिपि\n\n"
"Verum Script is a Lisp, so every word can be Sanskrit. परिभाषा is define, अस्तु is let,\n"
"आह्वानम् is call - the only thing a program can do to the world.\n"
"Type it on a Latin keyboard with `lipi on`: paribhASA becomes परिभाषा.\n\n"
"```\n"
"(परिभाषा (सन्देशे प्रेषकः विषयः)\n"
"  (अस्तु ((वस्तु (आह्वानम् भाण्डारः \"प्राप्\" विषयः)))\n"
"    (आह्वानम् पटलः \"मुद्रय\" \"शब्दाः\" (परिमाणम् (विभज (गृहाण वस्तु \"data\") \" \")))))\n"
"```\n\n"
"This page carries a cell: the Sanskrit word counter. Nothing runs until you say `allow`;\n"
"then it can do exactly what is listed and nothing else - it will count the words of this very page.\n\n"
"@cell ganana store[get,put] console[print]\n\n"
"=> tag:stats what it produced\n"
"=> page:home home\n";

const BuiltinPage BUILTIN_PAGES[] = { { "home", HOME }, { "about", ABOUT }, { "sanskrit", SANSKRIT } };
const int N_BUILTIN_PAGES = sizeof BUILTIN_PAGES / sizeof BUILTIN_PAGES[0];
