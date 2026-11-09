;;; export.el --- batch HTML export of the MICROCOSM docs  -*- lexical-binding: t -*-

;; The Makefile loads this before exporting each page.  The HTML is
;; kept next to the .org files, so the same .org file has to give the
;; same .html every time.

(require 'subr-x)
(require 'ucs-normalize)
(require 'ox-html)

;; Prism highlights source blocks in the browser.
(setq org-html-htmlize-output-type nil)

;;; Heading ids

;; Every heading keeps its id in a CUSTOM_ID property, which Org uses
;; as the HTML id.  That way a link to a section survives a rebuild
;; and a renamed heading.  make ids fills in the missing ones with
;; microcosm-docs-add-custom-ids.

(defun microcosm-docs-slug (title)
  "Turn heading TITLE into a kebab-case id.
Possessives, link and math markup and other punctuation are dropped."
  (let* ((s (downcase (ucs-normalize-NFKD-string title)))
         ;; Accents decompose into a letter and a combining mark.
         (s (replace-regexp-in-string "[̀-ͯ]" "" s))
         ;; [[target][description]] keeps the description.
         (s (replace-regexp-in-string
             "\\[\\[\\(?:[^]]*\\]\\[\\)?\\([^]]*\\)\\]\\]" "\\1" s))
         (s (replace-regexp-in-string "['’]s\\b" "" s))
         (s (replace-regexp-in-string "['’]" "" s))
         (s (replace-regexp-in-string "[^a-z0-9]+" "-" s))
         (s (string-trim s "-+" "-+")))
    (if (string-empty-p s) "section" s)))

(defun microcosm-docs--unique (base taken)
  "Return BASE, or BASE-2, BASE-3 and so on, whichever is not in TAKEN."
  (let ((id base)
        (n 1))
    (while (member id taken)
      (setq n (1+ n)
            id (format "%s-%d" base n)))
    id))

(defun microcosm-docs--indented-p ()
  "Non-nil when the buffer indents body text under its headings."
  (save-excursion
    (goto-char (point-min))
    (and (re-search-forward org-heading-regexp nil t)
         (re-search-forward "^[ \t]*[^ \t\n]" nil t)
         (not (org-at-heading-p))
         (> (current-indentation) 0))))

(defun microcosm-docs-add-custom-ids ()
  "Give every heading of the current buffer without a CUSTOM_ID one."
  (let ((org-adapt-indentation (microcosm-docs--indented-p))
        (taken (org-property-values "CUSTOM_ID"))
        (headings nil))
    (org-map-entries (lambda () (push (point-marker) headings)))
    (dolist (m (nreverse headings))
      (goto-char m)
      (unless (org-entry-get nil "CUSTOM_ID")
        (let ((id (microcosm-docs--unique
                   (microcosm-docs-slug (org-get-heading t t t t)) taken)))
          (push id taken)
          (org-entry-put nil "CUSTOM_ID" id)))))
  (when (buffer-modified-p)
    (save-buffer)))

;; A heading added since the last make ids still gets an id from its
;; text rather than a random one.
(defun microcosm-docs--reference (orig datum info)
  (if (not (eq (org-element-type datum) 'headline))
      (funcall orig datum info)
    (let ((cache (plist-get info :internal-references)))
      (or (org-element-property :CUSTOM_ID datum)
          (car (rassq datum cache))
          (let ((ref (microcosm-docs--unique
                      (microcosm-docs-slug (org-element-property :raw-value datum))
                      (append (org-element-map (plist-get info :parse-tree)
                                  'headline
                                (lambda (h) (org-element-property :CUSTOM_ID h)))
                              (mapcar #'car cache)))))
            (plist-put info :internal-references (cons (cons ref datum) cache))
            ref)))))

(advice-add 'org-export-get-reference :around #'microcosm-docs--reference)

;; The few other elements that get an id, such as example blocks, still
;; go through `random'.  A fixed seed keeps those stable too.
(random "microcosm-docs")

;;; export.el ends here
