;; A piece whose error falls inside the standard library: chain with no
;; effects. The client names the piece's call that led there.
(import "std/effects")

(defpiece main ()
  (let ((in (pipe :audio)) (out (pipe :audio)))
    (page (chain in out))))
