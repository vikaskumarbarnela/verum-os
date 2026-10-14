; wordcount - turns a note into a stats object, with provenance.
; Needs: store[get,put]   console[print]
(define (count-words text)
  (len (split text " ")))

(define (on-message from body)
  (let ((obj   (call store "get" body))
        (text  (get obj "data"))
        (stats (dict "words" (count-words text)
                     "chars" (len text)
                     "lines" (len (split text "\n")))))
    (call console "print" "counted" (get stats "words") "words in" (slice body 0 12))
    (call store "put" "stats" stats (list "stats") (list (get obj "hash")))))
