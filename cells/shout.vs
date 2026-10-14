; shout - upper-cases a note and forwards the new object's hash to another cell.
; Needs: store[get,put]   cell:<target>[send]   (bound as `next`)
(define (on-message from body)
  (let ((obj (call store "get" body))
        (h   (call store "put" "note" (upper (get obj "data")) (list "note" "shouted") (list (get obj "hash")))))
    (call next "send" h)))
