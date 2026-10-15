/* Cells shipped inside the kernel image. There is no filesystem to load code
   from: code is just another content-addressed object in the store. */
#include "verum.h"

static const char WORDCOUNT[] =
"; wordcount - turns a note into a stats object, with provenance.\n"
"(define (count-words text) (len (split text \" \")))\n"
"(define (on-message from body)\n"
"  (let ((obj   (call store \"get\" body))\n"
"        (text  (get obj \"data\"))\n"
"        (stats (dict \"words\" (count-words text) \"chars\" (len text))))\n"
"    (call console \"print\" \"counted\" (get stats \"words\") \"words in\" (slice body 0 12))\n"
"    (call store \"put\" \"stats\" stats (list \"stats\") (list (get obj \"hash\")))))\n";

static const char SHOUT[] =
"; shout - upper-cases a note and forwards the new object's hash to `next`.\n"
"(define (on-message from body)\n"
"  (let ((obj (call store \"get\" body))\n"
"        (h   (call store \"put\" \"note\" (upper (get obj \"data\")) (list \"note\" \"shouted\") (list (get obj \"hash\")))))\n"
"    (call next \"send\" h)))\n";

static const char MALWARE[] =
"; malware - a hostile cell. Grant it only: console print\n"
"(define (spin n) (map (lambda (i) (map (lambda (j) (* i j)) (range n))) (range n)))\n"
"(define (grow l) (grow (append l l)))\n"
"(define (deep i) (+ 1 (deep (+ i 1))))\n"
"(define (on-message from n)\n"
"  (cond\n"
"    ((= n 1) (call console \"print\" \"attack 1: forge a capability from a guessed token\")\n"
"             (call \"deadbeefdeadbeefdeadbeefdeadbeef\" \"get\" \"anything\"))\n"
"    ((= n 2) (call console \"print\" \"attack 2: use my console cap to read the store\")\n"
"             (call console \"get\" \"anything\"))\n"
"    ((= n 3) (call console \"print\" \"attack 3: burn CPU forever\")\n"
"             (spin 2000))\n"
"    ((= n 4) (call console \"print\" \"attack 4: allocate until memory is gone\")\n"
"             (grow (list 1)))\n"
"    ((= n 5) (call console \"print\" \"attack 5: blow the stack\")\n"
"             (deep 0))\n"
"    (else (call console \"print\" \"no such attack\"))))\n";

static const char ECHO[] =
"; echo - prints whatever it receives, with the sender's name.\n"
"(define (on-message from body) (call console \"print\" from \"says\" body))\n";

static const char DELEG[] =
"; deleg - passes a narrower copy of its own store capability to the cell bound as `next`.\n"
"; Needs: store[get,delegate]  cell->x[send] as next.  Message body: the rights to pass, e.g. \"get\".\n"
"(define (on-message from rights)\n"
"  (call store \"delegate\" next rights \"store\")\n"
"  (call next \"send\" (str \"you now hold store[\" rights \"] from me\")))\n";

static const char WORKER[] =
"; worker - a long computation: n*n products, then a report. Needs: console[print]\n"
"(define (work n) (reduce + 0 (map (lambda (i) (reduce + 0 (map (lambda (j) (* i j)) (range n)))) (range n))))\n"
"(define (on-message from n)\n"
"  (call console \"print\" \"starting\" n)\n"
"  (let ((total (work n)))\n"
"    (call console \"print\" \"finished: sum of\" (* n n) \"products =\" total)))\n";

/* संस्कृतलिपि: the same word counter, written in Sanskrit. The framebuffer console draws its Devanagari output. */
static const char GANANA[] =
"(परिभाषा (शब्दगणना वाक्यम्) (परिमाणम् (विभज वाक्यम् \" \")))\n"
"(परिभाषा (सन्देशे प्रेषकः विषयः)\n"
"  (अस्तु ((वस्तु (आह्वानम् भाण्डारः \"प्राप्\" विषयः))\n"
"          (लेखः  (गृहाण वस्तु \"data\"))\n"
"          (फलम्  (कोशः \"words\" (शब्दगणना लेखः) \"chars\" (परिमाणम् लेखः))))\n"
"    (आह्वानम् पटलः \"मुद्रय\" \"शब्दाः\" (गृहाण फलम् \"words\") \"अक्षराणि\" (गृहाण फलम् \"chars\"))\n"
"    (आह्वानम् भाण्डारः \"स्थापय\" \"stats\" फलम् (सूची \"stats\") (सूची (गृहाण वस्तु \"hash\")))))\n";

const BuiltinCell BUILTIN_CELLS[] = {
    { "ganana", GANANA },
    { "worker", WORKER },
    { "wordcount", WORDCOUNT }, { "shout", SHOUT }, { "malware", MALWARE }, { "echo", ECHO }, { "deleg", DELEG },
};
const int N_BUILTIN_CELLS = sizeof BUILTIN_CELLS / sizeof BUILTIN_CELLS[0];
