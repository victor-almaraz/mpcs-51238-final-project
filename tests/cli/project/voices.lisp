;; Imported by main.lisp in the command-line tests.
(module voices (export tone))
(define (tone freq out) (sine-osc :freq freq :out out))
