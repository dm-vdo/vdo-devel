;; -*- lexical-binding: t -*-

;; restyle-declarations-in-files

;; There are still kernel constructs this doesn't handle, like "static
;; DEFINE_THING(some,args,here);" but it's close enough for a first
;; cut.

;; There are plenty of other cases where scanning for decl arguments
;; lands us someplace odd like in enum definitions, which we mostly
;; manage to log and skip over.

;; There are still kernel constructs this doesn't recognize properly,
;; like "static DEFINE_THING(some,args,here);" variable definitions
;; but it's close enough for a first cut. They get reflowed and that's
;; probably fine.
;;
;; There are other cases where scanning for decl arguments lands us
;; someplace odd like in enum definitions, which we mostly manage to
;; report and skip over.
;;
;; logger.h:
;;   Macro uds_log_ratelimit confuses this code as it has a DEFINE_
;;   macro use that looks kind of like a function decl, and there's no
;;   exception in here to leave macro definitions alone.
;; uds-threads.h:
;;   __attribute__((noreturn)) confuses the parsing and needs to be
;;   temporarily removed while working on the file.
;; encodings.h:
;;   blank line with whitespace gets inserted in an enum list

;; restyle-funcalls-in-files

;; Re-wraps function calls after arg-list commas. Sometimes this
;; uglifies things, usually in expressions like
;; "f1(arg1,arg2)&&f2(arg3,arg4)" where breaking at the binary
;; operator would be nicer, or function calls that are themselves
;; within argument lists.

;; Also, function call as initializer in a compound expression
;; (".x=foo(arg1,arg2)") often winds up with a blank line inserted
;; after it.

;; Most function calls are rewrapped pretty cleanly.  But some manual
;; review of the results is needed.

(require 'array) ;; for current-line
(require 'subr-x)
(require 'text-property-search)

(defvar debug-reflow nil)

(defun save-must-check-annotations ()
  "Replace \"__must_check\" with a text property.

The Emacs C syntactic analysis doesn't understand __must_check,
so it must be hidden. A text property must-check-sep is attached
to the following token with the whitespace separator following
__must_check."
  (save-excursion
    (goto-char (point-min))
    (while (re-search-forward "__must_check\\([ \n]+\\)" nil t)
      (let ((post-ws (match-string-no-properties 1)))
	(put-text-property (+ 1 (match-end 0))
			   (+ 2 (match-end 0))
			   'must-check-sep
			   post-ws)
	(delete-region (match-beginning 0) (match-end 0))))
    (font-lock-fontify-region (point-min) (point-max))
    ))

(defun restore-must-check-annotations-in-region (start end)
  "Restore __must_check tokens in region from START to END.

Find any must-check-sep text properties in the region and remove
them, inserting the previously removed __must_check tokens and
whitespace."
  (save-excursion
    (goto-char start)
    (let (match)
      (while (and (setq match (text-property-search-forward 'must-check-sep))
		  (< (prop-match-end match) end))
	(goto-char (1- (prop-match-beginning match)))
	(let ((sep (prop-match-value match)))
	  (remove-text-properties (prop-match-beginning match)
				  (prop-match-end match)
				  '(must-check-sep))
	  (insert "__must_check" sep))
	))))

(defun restore-must-check-annotations ()
  "Restore __must_check tokens in the buffer."
  (restore-must-check-annotations-in-region (point-min) (point-max)))

(defconst external-static-marker
  (let ((s (copy-sequence "static")))
    (add-text-properties 0 1
			 '(is-external-static t)
			 s)
    s))

(defun save-external-static ()
  "Find uses of the \"STATIC\" macro and replace them with \"static\".

Attach a text property to the new text so the replacement
locations can be identified later.

(The function name is a throwback to the old spelling of the C
macro.)"
  (save-excursion
    (goto-char (point-min))
    (let ((case-fold-search nil))
      (while (re-search-forward "\\_<STATIC\\_>" nil t)
	(replace-match external-static-marker t t)))))

(defun restore-external-static ()
  "Restore \"STATIC\" macro uses previously changed to \"static\".

Searches for the is-external-static text property attached to the
\"static\" keyword and replaces the keyword with \"STATIC\".

(The function name is a throwback to the old spelling of the C
macro.)"
  (save-excursion
    (goto-char (point-min))
    (let (match)
      (while (setq match (text-property-search-forward 'is-external-static))
	(goto-char (prop-match-beginning match))
	(if debug-reflow
	    (message "restore %S %S" (point) match))
	(if (not (looking-at "static"))
	    (error "is-external-static marker not at static keyword?? %S" (point)))
	(delete-char 6)
	(insert "STATIC")))))

(defun beginning-of-line-pos ()
  (save-excursion
    (beginning-of-line)
    (point)))

(defun end-of-line-pos ()
  (save-excursion
    (end-of-line)
    (point)))

(defun current-line-as-string ()
  ;; Just what it says on the tin.
  (buffer-substring-no-properties (beginning-of-line-pos) (end-of-line-pos)))

(defun buffer-substring-near-point (chars-before chars-after)
  (let ((here (point)))
    (buffer-substring-no-properties (max (point-min)
					 (- here chars-before))
				    (min (point-max)
					 (+ here chars-after)))))

(defun squash-function-decl-to-one-line (start-pos end-pos)
  (save-restriction
    (narrow-to-region start-pos end-pos)
    ;; A "*" followed by newline can be squashed together, but
    ;; non-punctuation tokens followed by newline followed by more
    ;; stuff should keep some whitespace.
    (goto-char start-pos)
    (while (re-search-forward "\\*[\n\t ]*\n[\n\t ]*" nil t)
      (replace-match "*" t t))
    (goto-char (point-min))
    (while (re-search-forward "[\n\t ]+" nil t)
      (if (not (string-equal " " (match-string 0)))
	  (replace-match " ")))
    (goto-char (point-max))
    ;; Remove trailing whitespace
    (while (looking-back "[\n\t ]" nil)
      (delete-char -1))))

(defun search-nonstring (search-fn string bound)
  "Similar to search-backward etc but skips strings found inside string literals.

Assumes NOERROR is t and COUNT is unspecified.  Requires a fully
fontified buffer."
  (if (funcall search-fn string bound t)
      (let (at-bound)
	(while (and (not at-bound)
		    (eq 'font-lock-string-face
			(plist-get (text-properties-at (match-beginning 0)) 'face)))
	  ;; We're looking at a string literal; skip it and keep
	  ;; searching.
	  (if (not (funcall search-fn string bound t))
	      (setq at-bound t)))
	;; On exit from the while loop, at-bound is t if we hit the
	;; limit, nil if we found a match not in a string literal.
	(not at-bound))))

(defun search-backward-nonstring (string bound)
  "Similar to search-backward but skips strings found inside string literals.

Assumes NOERROR is t and COUNT is unspecified.  Requires a fully
fontified buffer."
  (search-nonstring #'search-backward string bound))

(defun search-forward-nonstring (string bound)
  "Similar to search-forward but skips strings found inside string literals.

Assumes NOERROR is t and COUNT is unspecified.  Requires a fully
fontified buffer."
  (search-nonstring #'search-forward string bound))

(defun re-search-forward-nonstring (string bound)
  "Similar to search-forward but skips strings found inside string literals.

Assumes NOERROR is t and COUNT is unspecified.  Requires a fully
fontified buffer."
  (search-nonstring #'re-search-forward string bound))

(defun break-line-at-commas ()
  "Break at commas to try to keep the line length under fill-column.

The fill-column target is treated as soft; if the first comma is
past that column or there are no commas, the line will exceed the
target width."
  (let (done)
    (while (and (not done)
		(> (current-column) fill-column))
      (move-to-column fill-column)
      (if (search-backward-nonstring "," (beginning-of-line-pos))
	  (progn
	    (forward-char 1)
	    (insert "\n")
	    (indent-for-tab-command))
	(if debug-reflow
	    (message "too long but no comma to left? %S %S"
		     (point)
		     (current-line-as-string)))
	(if (search-forward-nonstring "," (end-of-line-pos))
	    (progn
	      (insert "\n")
	      (indent-for-tab-command))
	  (setq done t)))
      (end-of-line))))

(defun reflow-function-decl (start-pos end-pos)
  "Reflow a function declaration or function definition header.

The function declaration must be enclosed in the region from
START-POS to END-POS, without additional non-whitespace text.
Break the argument list at commas to keep lines under
`fill-column' when possible."
  (save-restriction
    (narrow-to-region start-pos end-pos)
    (squash-function-decl-to-one-line start-pos end-pos)
    (break-line-at-commas)))

(defun examine-non-void-decls ()
  "Find function declarations with arguments and reflow the line breaks.

Function definition headers are processed also.

The return value is information from each declaration processed,
intended for debugging purposes only."
  ;; Returns a list of strings, the regions that have been tweaked,
  ;; mostly for debugging purposes. When not debugging, it's just
  ;; noise.
  (font-lock-fontify-region (point-min) (point-max))
  (goto-char (point-min))
  (let (start-limit start-pos end-pos
	(result))
    (while (c-search-forward-char-property 'c-type 'c-decl-arg-start)
      (up-list)
      (setq end-pos (point))
      (backward-sexp)
      ;; We're positioned after the function (or typedef?) name
      (if nil
	  (message "examining %d %S"
		   (point)
		   (buffer-substring-near-point 5 20)))
      (cond ((looking-back "=[ \t\n]*(*" nil)
	     ;; Some enum value assignments get assigned a
	     ;; c-decl-arg-start type for some reason.  Just skip
	     ;; them.
	     )
	    ((looking-back ")" nil)
	     (backward-sexp))
	    ((c-simple-skip-symbol-backward)
	     ;; Now at first character of function name; skip back
	     ;; over tokens that look like the return type etc.
	     (setq start-pos (point))
	     (let (done)
	       (while (not done)
		 (c-backward-syntactic-ws start-limit)
		 (cond ((and start-limit
			     (<= (point) start-limit))
			(setq done t))
		       ((looking-back "[a-zA-Z0-9_]+" start-limit)
			(c-simple-skip-symbol-backward)
			(setq start-pos (point)))
		       ((looking-back "\\*" start-limit)
			(backward-char 1)
			(setq start-pos (point)))
		       ((looking-back "[;}]" start-limit)
			(setq done t))
		       (t
			(message "what am i looking back at? %S/%S %S"
				 (point)
				 (current-line)
				 (buffer-substring-near-point 10 20))
			(setq done t)))))
	     (goto-char end-pos)
	     (end-of-line 1)
	     (setq end-pos (point))
	     (goto-char start-pos)
	     (save-restriction
	       (narrow-to-region start-pos end-pos)
	       (restore-must-check-annotations-in-region start-pos end-pos)
	       (if (not (point-in-macro-p))
		   (reflow-function-decl (point-min) (point-max)))
	       (goto-char (point-max)))
	     (let ((lst (list (point)
			      (current-line)
			      (buffer-substring-no-properties start-pos
							      (min (point-max)
								   (+ end-pos 1))))))
	       (push (if t lst end-pos) result)))
	    (t
	     (error "not at a symbol? %S %S"
		    (point)
		    (buffer-substring-near-point 5 20))))
      (setq start-limit end-pos)
      (goto-char end-pos)
      )
    (nreverse result)))

(defun examine-void-decls ()
  ;; To do: Look for "(void)" and still adjust return type line breaks
  ;; etc.
  ;;
  ;; Not handled yet.  Luckily all our no-argument functions already
  ;; seem to use the one-line style.
  )

(defun examine-decls ()
  "Find function declarations and reflow the line breaks.

Function definition headers are processed also."
  (examine-void-decls)
  (examine-non-void-decls))

;; reflow the current buffer
(defun reflow-current-buffer ()
  (let ((result)
	(fill-column 79))
    (save-external-static)
    (save-must-check-annotations)
    ;;(font-lock-fontify-region (point-min) (point-max))
    (setq result (examine-decls))
    (restore-must-check-annotations)
    (restore-external-static)
    result))

;; for interactive testing
(defun run-test ()
  (message "%S" (reflow-current-buffer)))

(defun apply-buffer-op-to-argv-files (operation)
  (let (filename (failed-files nil))
    (while (setq filename (pop argv))
      (message "Processing %s..." filename)
      (find-file filename)
      (c-mode)
      (condition-case x
	  (funcall operation)
	(error
	 (message "error processing %s : %S"
		  filename x)
	 (push filename failed-files))
	(:success
	 (message "Saving %s..." filename)
	 (basic-save-buffer))
	(t
	 (message "unhandled signal in %s: %S"
		  filename x)))
      (kill-buffer))
    (if failed-files
	(message "Errors processing files: %s"
		 (string-join (nreverse failed-files) " ")))
    (kill-emacs (if failed-files 1 0))))

;; command line
;; emacs --batch -Q -l thisfile.el -f restyle-declarations-in-files file1.c file2.h ...
(defun restyle-declarations-in-files ()
  (apply-buffer-op-to-argv-files #'reflow-current-buffer))

;; function calls

(defun reflow-function-call ()
  (save-restriction
    (let ((start (point)))
      (forward-sexp 1)
      (narrow-to-region start (point))
      (goto-char (point-min))
      ;; Skip reflowing if there aren't any commas.
      (if (search-forward-nonstring "," nil)
	  (progn
	    (goto-char (point-min))
	    (while (re-search-forward "[\t ]*\n[\t ]*" nil t)
	      (replace-match " " t t))
	    (goto-char (point-min))
	    (let ((end (point-max)))
	      ;; Restore preceding context to get proper indentation levels.
	      (widen)
	      ;; Add 1 to factor in ";" if present.
	      (narrow-to-region (point-min) (+ 1 end)))
	    ;; Note that this won't introduce line breaks for
	    ;; "foo(arg1,arg2)+17" if the "+17" runs over the length
	    ;; limit, because we're still limiting the processing to
	    ;; the argument list.
	    (goto-char (point-max))
	    (break-line-at-commas)))
      ;; Always move to the end of the argument list before widening,
      ;; so that we can search forward for a second function call on
      ;; the same line, if present.
      (goto-char (point-max)))))

(defun find-next-function-body ()
  "Find the next function-body start.

If found, sets point to the opening { and returns t; otherwise
returns nil but may have adjusted point anyway."
  ;; keep looping until defun-open found
  (while (and (search-forward "{" nil 0)
	      (not (assq 'defun-open (c-guess-basic-syntax)))))
  (if (looking-back "{" (- (point) 1))
      (progn
	(backward-char 1)
	t)))

(defun point-in-macro-p ()
  (assq 'cpp-macro
	(save-restriction
	  ;; Guessing syntax sometimes loops recursively if it wants
	  ;; more context but we're narrowed.
	  (widen)
	  (c-guess-basic-syntax))))

(defun reflow-function-calls-in-buffer ()
  (font-lock-fontify-region (point-min) (point-max))
  (goto-char (point-min))
  (setq fill-column 79)
  (while (find-next-function-body)
    (save-restriction
      (narrow-to-defun)
      ;; Include ")" to handle "(foo->callback)(args)"
      (while (re-search-forward-nonstring "[a-zA-Z0-9)](" nil)
	(if (not (point-in-macro-p))
	    (progn
	      ;; Position at the "(" rather than after it
	      (backward-char 1)
	      (reflow-function-call)))
	)
      ;; Reposition so next search will start after this function.
      (goto-char (point-max)))))

(defun restyle-funcalls-in-files ()
  (apply-buffer-op-to-argv-files #'reflow-function-calls-in-buffer))
