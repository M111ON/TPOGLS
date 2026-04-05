/* 4. Audit buffer accumulate (bounded)
 * NOTE:
 *   rh.do_audit may not fire every 8 ops for every route pattern.
 *   Keep pos bounded to avoid writing past audit.buf[8]. */

if (p->audit.pos >= GEO_GROUP_SIZE) {
        _audit_buf_reset(&p->audit, a.spoke);
    }

if (rh.do_audit || p->audit.pos == GEO_GROUP_SIZE) {