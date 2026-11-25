; Datum comments (highlighter.cpp, skipDatum) and comments over lines.
#; (commented (out
  over lines) "and \q a bad escape")
(kept #;dropped kept)
#; #; one two
#; 'quoted after
#; ; a comment first
  next
#; #| a block |# then
#|
  a block comment
  #| nested
  |# still inside
|#
"a string
over lines"
#;"a commented string
over lines" after
(define
  name-on-next-line 1)
#;(unclosed in the middle
(the rest of the file is commented
