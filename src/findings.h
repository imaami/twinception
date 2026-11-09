/** @file findings.h
 *
 * The findings tool, which askai lends only to the turns of an ask call,
 * with --notes. A call adds a finding of the agent (src/finding.h),
 * updates one, lists the findings of every member, or shows one finding
 * with its changes. The evidence of an add or an update comes from the
 * calls of the ring of the ask call (src/evidence.h, src/seen.h).
 *
 * The findings of an agent are the branch refs/heads/agent/NAME/findings
 * of the session repository (src/store.h). Each add and each update is one
 * commit on that branch, made at once with the file of the finding and the
 * archive files of its evidence. The number of a new finding is one more
 * than the highest number on the branch when the commit is made. If
 * another ask call of the agent changed the branch at the same time, the
 * call reads the branch again and makes the commit again, STORE_TRIES
 * commits at most. The result names the ID only after the commit.
 *
 * git reads the files of findings in runs of cat-file whose output stays
 * within STORE_MAX bytes, so that any number of findings can be read.
 *
 * The tool is a built-in plugin without a part. Its service is the struct
 * notes_tool of the notes tool (src/notes_tool.h), which contains the
 * store and the members. The notes tool adds the open findings of the
 * agent to the prompt of the turns (findings_read_input(),
 * findings_notice()).
 */
#ifndef ASKAI_SRC_FINDINGS_H_
#define ASKAI_SRC_FINDINGS_H_

#include <stddef.h>

#include "buf.h"

struct dstr;
struct hexid;
struct plugin;

/** The plugin of the findings tool, which is built into askai. */
extern struct plugin const findings_plugin;

/**
 * Writes to @p in the input of STORE_READ for the next files of findings
 * in @p *files, the rest of the output of STORE_FILES for a branch of
 * findings and the path findings: as many files as fit in one output of
 * STORE_READ (store_read_fits()), at least one. Drops those files, and the
 * files that are not files of findings, from @p *files. Returns 0, with
 * @p in empty if no file of a finding is left, EIO for output that is not
 * that of STORE_FILES, or ENOMEM.
 */
extern int
findings_read_input (struct buf  *in,
                     struct span *files);

/** Appends to @p d the notices of the open findings of the agent in the
 *  conversation @p conversation (finding_notice()), at most @p most bytes
 *  of lines of findings, from @p out, the outputs of STORE_READ for the
 *  inputs of findings_read_input(), one after another. Returns 0, EIO for
 *  output that is not the files of findings, or ENOMEM.
 */
extern int
findings_notice (struct span         out,
                 struct hexid const *conversation,
                 size_t              most,
                 struct dstr        *d);

#endif /* ASKAI_SRC_FINDINGS_H_ */
