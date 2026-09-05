/*
 * sharc_exec.h — ADSP-21060 SHARC HLE command dispatch and handlers.
 *
 * Analogous to i960_exec.h: this file contains the argument-count table and
 * the command executor (the big switch).  All SHARC state lives in g_sharc
 * (sharc.h); cop.h owns only the i960↔SHARC FIFO interface.
 *
 * MAME verified against fvipers (inputs/outputs confirmed against live MAME fvipers via MCP bridge):
 *   0x03000606  set_pos: translate T += R*(x,y,z)         verified: 20/20 OK (via 0x07800F0F T tracking)
 *   0x04800909  set Y angle (post-multiply Ry)            verified: max_err < 1e-5
 *   0x06800D0D  zero T[0..2]                              verified: 20/20 OK (zero-reply after D0D)
 *   0x07800F0F  read world translation T[0..2]            verified: 20/20 OK
 *   0x09801313  fadd(a,b) → float                        verified: 20/20 OK
 *   0x0A001414  fsub(a,b) → float                        verified: 20/20 OK
 *   0x0B001616  fdiv(a,b) → float                        verified: 20/20 OK
 *   0x0B801717  int2f(arg0) → float                      verified: 20/20 OK (FV PM 0x02059D FLOAT R1)
 *   0x0D001A1A  sqrt(a) → float                          verified: 20/20 OK
 *   0x10802121  sin(i16 angle) → float                   verified: 20/20 OK
 *   0x11002222  cos(i16 angle) → float                   verified: 20/20 OK
 *   0x12002424  sin(i16)*float → float                   verified: 20/20 OK
 *   0x12802525  cos(i16)*float → float                   verified: 20/20 OK
 *   0x13802727  atan2(b,a) → i16 fixed-point angle       verified: 20/20 OK (tol ±1 unit)
 *   0x14802929  model→world: R*(x,y,z)+T → 3 floats      verified: 20/20 OK
 *   0x15802B2B  dist2D(x1,x2,z1,z2) → float             verified: 20/20 OK
 *   0x16002C2C  dist3D(x1,x2,y1,y2,z1,z2) → float       verified: 20/20 OK
 *   0x16802D2D  mag2D(a,b) → sqrt(a²+b²)                verified: 20/20 OK
 *   0x17802F2F  azimuth(z1,z2,x2,x1) → i16 angle        verified: 20/20 OK
 *   0x18803131  lerp(a,b,t,span) → a+(b-a)*t/span        verified: 20/20 OK (FV PM 0x020E80)
 *   0x1B003636  load bone cache → current matrix          firmware: PM 0x0204C2, plain copy
 *   0x1B803737  load bone cache + C×B multiply            firmware: PM 0x0204D6, calls 0x201E9
 *   0x1A803535  save current matrix → bone cache          firmware: PM 0x0204AE
 *   0x24804949  write COP internal address               verified: returns 0 at init
 *   0x35006A6A  world→model: R*(v−T) → 3 floats          verified: 20/20 OK
 *   0x2E005C5C  add_vec3(a0+a1, a2+a3, a4+a5) → 3 floats verified: 20/20 OK (FV PM 0x0206D6)
 *   0x2F005E5E  scale_vec3(s,x,y,z) → (s*x,s*y,s*z)     verified: 20/20 OK (FV PM 0x020702)
 *   0x3C007878  polygon submission                        verified: 19/19 hits pass
 *   0x41008282  query COP internal counter                verified: MAME returns 0 at init
 *   0x02800505  read 3×4 matrix (row-major)              verified: 12 floats row0..row2
 *   0x02000404  write 3×4 matrix (row-major)             inverse of 0x02800505
 *   0x2B805757  read rot col1 → 3 floats                 firmware: slot[3..5] (PM 0x02044E)
 *   0x2C005858  read rot col2 → 3 floats                 firmware: slot[6..8] (PM 0x020457)
 *   0x11802323  tan(i16 angle) → float                   firmware: sin/cos via RECIPS+NR (PM 0x02061C); NO DATA in attract
 */
#ifndef SHARC_EXEC_H
#define SHARC_EXEC_H

#include "sharc.h"

/* ---- Argument count table ------------------------------------------------ */

static inline int sharc_args_for_cmd(uint32_t cmd) {
    switch (cmd) {
        /* Setters */
        case 0x03000606: return 3;
        case 0x03800707: return 3;
        case 0x04000808: return 1;
        case 0x04800909: return 1;
        case 0x05000A0A: return 1;
        /* Math: 2-in / 1-out */
        case 0x12002424: return 2;
        case 0x12802525: return 2;
        /* Math: 6-in / 3-out */
        case 0x2E005C5C: return 6;
        /* 0x1A003434: object→polygon index lookup — 1 arg, 1 result */
        case 0x1A003434: return 1;
        /* snc_eye_thd_set camera transforms */
        case 0x18003030: return 3;
        case 0x13002626: return 1;
        /* Animation curve interpolation: 6-in / 1-out */
        case 0x19003232: return 6;
        /* Afterimage/zanzou slot reads */
        case 0x42808585: return 1;
        case 0x42008484: return 1;
        /* Shadow slot / matrix commands */
        case 0x39807373: return 2;
        case 0x3A807575: return 6;
        case 0x3A007474: return 7;
        case 0x05800B0B: return 12;
        case 0x23004646: return 1;
        /* COP internal memory (collision/afterimage data) */
        case 0x24004848: return 1;
        case 0x24804949: return 2;
        case 0x43008686: return 1;
        case 0x09801313: return 2;  /* fadd(a,b) → float  MAME-verified PM 0x0205B0 */
        case 0x0A001414: return 2;
        case 0x0A801515: return 2;
        case 0x0B001616: return 2;
        case 0x16802D2D: return 2;
        case 0x13802727: return 2;
        case 0x1B803737: return 2;
        case 0x1B003636: return 2;
        /* smooth_int: reset bone slot to identity + 9 ang ops (Z,Y,X,X,Y,Z,Y,Z,X) */
        case 0x2A005454: return 9;
        case 0x2A805555: return 9;  /* dispatch[0x55] PM 0x02115F — 9 args (3 triples) / 3 outputs. i960 sends opcode+3 stt then reads 3 (e.g. loop @0x2FB00). 0-arg desynced the FIFO. */
        case 0x08001010: return 0;  /* dispatch[0x10] PM 0x20460 — IDA: 0 args, 0 outputs (kira_kira_disp) */
        case 0x34006868: return 1;  /* dispatch[0x68] PM 0x205A3 — IDA: 1 arg, 0 outputs (name_char_kage_disp) */
        case 0x3B807777: return 4;  /* dispatch[0x77] PM 0x20B1F — IDA: 4 args (x,y,z,flags), 9 outputs */
        /* Collision / spatial — no FIFO output */
        case 0x1F003E3E: return 5;
        case 0x1D003A3A: return 4;
        case 0x1D803B3B: return 7;
        case 0x1E803D3D: return 6;
        case 0x39007272: return 3;
        case 0x38007070: return 5;
        /* Math: 1-in / 1-out */
        case 0x10802121: return 1;
        case 0x11002222: return 1;
        case 0x0B801717: return 1;
        case 0x0C001818: return 1;
        /* Spatial: 4-in / 1-out */
        case 0x15802B2B: return 4;
        case 0x17802F2F: return 4;
        case 0x2F005E5E: return 4;
        /* Vector transform: 3-in / 3-out */
        case 0x14802929: return 3;
        case 0x35006A6A: return 3;
        /* Bone slot write/select/flush — no FIFO output */
        case 0x07000E0E: return 3;  /* write 3 floats to bone slot at DM[0x3033F]+1,+2,+3 */
        case 0x1A803535: return 2;  /* save 12 words from bone slot to selection buffer */
        case 0x34806969: return 2;  /* load animation frame + build rotation matrix in bone slot */
        case 0x1C803939: return 4;  /* normalize 3 float args, write to bone buffer at arg4 offset */
        case 0x08801111: return 9;
        case 0x33806767: return 1;
        case 0x31006262: return 9;
        case 0x3F807F7F: return 1;  /* flush selection buffer (192 words) to output buffer */
        case 0x1C003838: return 1;  /* select bone data buffer (arg=1 → player 2, else player 1) */
        /* get_rot_matrix: read current 3×3 → 9 results */
        case 0x09001212: return 0;
        /* calc_rob_angle_cont: 2-bone IK (4×stt + stl + 3×st = 17 args) → 1 result */
        case 0x35806B6B: return 17;
        /* get_frame_dat: frame data lookup (stq + stt = 7 args) → 3 results */
        case 0x31806363: return 7;
        /* Polygon submission: 8-in / 2-out */
        case 0x3C007878: return 8;
        /* os_set_coli commands */
        case 0x22004444: return 1;
        case 0x2B005656: return 0;
        case 0x2D005A5A: return 2;
        case 0x2D805B5B: return 3;
        case 0x2C805959: return 4;
        /* write matrix: 12 row-major floats → loads rot[] and pos[] */
        case 0x02000404: return 12;
        /* Zero-arg ops */
        case 0x11802323: return 1;  /* tan(i16 angle): sin/cos via sincos+RECIPS — PM 0x02061C */
        case 0x2B805757: return 0;  /* read col1 of rotation: rot[1][0..2] → 3 floats — PM 0x02044E */
        case 0x2C005858: return 0;  /* read col2 of rotation: rot[2][0..2] → 3 floats — PM 0x020457 */
        case 0x25004A4A: return 1;  /* animation data reader: type-dispatched — PM 0x02076E */
        case 0x02800505: return 0;
        case 0x07800F0F: return 0;
        case 0x06000C0C: return 0;
        case 0x00800101: return 0;
        case 0x01000202: return 0;
        case 0x01800303: return 0;
        case 0x06800D0D: return 0;
        case 0x40808181: return 0;
        case 0x41008282: return 0;
        case 0x1F803F3F: return 3;  /* set_ang_xyz: 3 i16 angle args, 1 zero returned */
        case 0x21804343: return 1;
        case 0x22804545: return 1;
        /* fvipers commands (arg counts verified from PM 0x020670/0x020E80/0x020EF5/0x0205AF) */
        case 0x16002C2C: return 6;  /* 3D distance: (x1,x2,y1,y2,z1,z2) → 1 float */
        case 0x18803131: return 4;  /* interpolation: 4 floats → 1 float */
        case 0x28805151: return 0;  /* 0-arg: reads bone slot, pushes 3 results */
        case 0x0D001A1A: return 1;  /* sqrt(arg0) → 1 float — PM 0x0205AF */
        default:         return 0;
    }
}

/* ---- Command executor ---------------------------------------------------- */

static inline void sharc_exec(uint32_t cmd, const uint32_t *args, int n) {
#define SANITIZE(r) do { if (!((r)==(r)) || (r)>1e30f || (r)<-1e30f) (r)=0.0f; } while(0)

    g_sharc.reply_count = 0;
    g_sharc.reply_idx   = 0;

    switch (cmd) {
        /* ---- Setters ---- */
        case 0x03000606:
            /* SHARC firmware: T += rot × args  (no z-negation — uses raw column-major multiply). */
            g_sharc.ip_set_pos = g_last_store_ip;
            if (n >= 3) {
                float vx = sharc_bits_to_float(args[0]);
                float vy = sharc_bits_to_float(args[1]);
                float vz = sharc_bits_to_float(args[2]);
                float (*r)[3] = g_sharc.rot;
                g_sharc.pos[0] += r[0][0]*vx + r[1][0]*vy + r[2][0]*vz;
                g_sharc.pos[1] += r[0][1]*vx + r[1][1]*vy + r[2][1]*vz;
                g_sharc.pos[2] += r[0][2]*vx + r[1][2]*vy + r[2][2]*vz;
                g_sharc.matrix_dirty = true;
                g_sharc.bone_dirty   = true;
            }
            return;
        case 0x03800707:
            /* Firmware PM 0x02016E: scale each column of the current bone slot
             * in-place: rot[col][row] *= args[col] for all rows. */
            if (n >= 3) {
                for (int _col = 0; _col < 3; _col++) {
                    float _s = sharc_bits_to_float(args[_col]);
                    for (int _row = 0; _row < 3; _row++)
                        g_sharc.rot[_col][_row] *= _s;
                }
                g_sharc.matrix_dirty = true;
                g_sharc.bone_dirty   = true;
            }
            return;
        case 0x04000808:
            g_sharc.ip_set_ang_x = g_last_store_ip;
            if (n >= 1) {
                g_sharc.ang[0] = (int32_t)args[0];
                { float a = sharc_angle_to_rad(g_sharc.ang[0]);
                  sharc_postmul_rx(cosf(a), sinf(a)); }
                g_sharc.matrix_dirty = true;
                g_sharc.bone_dirty   = true;
            }
            return;
        case 0x04800909:  /* set Y angle + snapshot world_pos */
            g_sharc.ip_set_ang_y = g_last_store_ip;
            if (n >= 1) {
                g_sharc.ang[1] = (int32_t)args[0];
                { float a = sharc_angle_to_rad(g_sharc.ang[1]);
                  sharc_postmul_ry(cosf(a), sinf(a)); }
                g_sharc.matrix_dirty = true;
                g_sharc.bone_dirty   = true;
                g_sharc.world_pos[0] = g_sharc.pos[0];
                g_sharc.world_pos[1] = g_sharc.pos[1];
                g_sharc.world_pos[2] = g_sharc.pos[2];
            }
            return;
        case 0x05000A0A:
            g_sharc.ip_set_ang_z = g_last_store_ip;
            if (n >= 1) {
                g_sharc.ang[2] = (int32_t)args[0];
                { float a = sharc_angle_to_rad(g_sharc.ang[2]);
                  sharc_postmul_rz(cosf(a), sinf(a)); }
                g_sharc.matrix_dirty = true;
                g_sharc.bone_dirty   = true;
            }
            return;
        case 0x06000C0C:
            return;
        case 0x06800D0D:
            /* STF PM 0x02042A / FV PM 0x0203EF: `DM(I7, 0x09)` post-modify-by-9
             * advances I7 from slot[0] to slot[9]=T[0], then zeros slot[9..11].
             * Both firmwares share hex 0x00006E7E48000000 for this instruction —
             * the "+9" is in bits[31:27] of the lower instruction word.
             * CLAUDE.md had this wrong (said "zeros slot[1..3]"); zeros T[] is correct. */
            g_sharc.pos[0] = 0.0f;
            g_sharc.pos[1] = 0.0f;
            g_sharc.pos[2] = 0.0f;
            g_sharc.matrix_dirty = true;
            return;

        /* ---- Readback ---- */
        case 0x07800F0F:
            /* STF PM 0x02043D / FV PM 0x020402: same DM(I7, 0x09) post-modify-by-9
             * positions I7 at slot[9]=T[0]; LCNTR=3 loop outputs T[0..2].
             * Both firmwares are identical. CLAUDE.md had "slot[1..3]=rotation" wrong;
             * the STF annotation correctly named this read_world_pos.
             * The i960 stores T[] to g7+0x1F4 for collision/IK purposes. */
            sharc_push_f(g_sharc.pos[0]);
            sharc_push_f(g_sharc.pos[1]);
            sharc_push_f(g_sharc.pos[2]);
            return;
        case 0x02800505:  /* read 3×4 matrix (12 floats) — row-major order */
            g_sharc.ip_read_matrix = g_last_store_ip;
            if (g_sharc.matrix_dirty) sharc_build_matrix();
            for (int row = 0; row < 3; row++)
                for (int col = 0; col < 4; col++)
                    sharc_push_f(g_sharc.matrix[row][col]);
            g_sharc.matrix_read_count++;
            return;

        /* 0x05800B0B: pre-multiply current bone by incoming 3×4 matrix — PM 0x203E9.
         * Firmware reads 12 column-major floats from FIFO → PM, then calls _L201E9
         * (mat×mat multiply: incoming × current → current). Same formula as 0x1B803737
         * but the left-factor B comes from the FIFO args rather than the bone cache.
         * Called by rob_kage_disp_test (shadow rendering) to apply a shadow projection
         * matrix to the current character bone. */
        case 0x05800B0B: {
            if (n >= 12) {
                const uint32_t *B_bits = args;  /* 12 column-major floats */
                float (*r)[3] = g_sharc.rot;
                float nr[3][3], np[3];
                for (int j = 0; j < 3; j++)
                    for (int i = 0; i < 3; i++) {
                        float s = 0.0f;
                        for (int k = 0; k < 3; k++)
                            s += sharc_bits_to_float(B_bits[j*3+k]) * r[k][i];
                        nr[j][i] = s;
                    }
                for (int i = 0; i < 3; i++) {
                    float s = g_sharc.pos[i];
                    for (int k = 0; k < 3; k++)
                        s += sharc_bits_to_float(B_bits[9+k]) * r[k][i];
                    np[i] = s;
                }
                for (int j = 0; j < 3; j++)
                    for (int i = 0; i < 3; i++) r[j][i] = nr[j][i];
                g_sharc.pos[0] = np[0];
                g_sharc.pos[1] = np[1];
                g_sharc.pos[2] = np[2];
                g_sharc.matrix_dirty = true;
                g_sharc.bone_dirty   = true;
            }
            return;
        }
        case 0x02000404: {
            /* set_matrix: restore 12 row-major floats into rot[] and pos[].
             * Inverse of 0x02800505: undo z-negation stored in our row-major format.
             *   m[r][0..1] = rot[0..1][r]  → rot[c][r] = m[r][c]
             *   m[r][2]    = -rot[2][r]    → rot[2][r] = -m[r][2]  for r=0,1
             *   m[2][2]    =  rot[2][2]    → rot[2][2] =  m[2][2]
             * SHARC firmware handler at 0x203AA reads 12 FIFO words → DM[DM[0x3033F]]. */
            if (n >= 12) {
                float (*r)[3] = g_sharc.rot;
                int row;
                for (row = 0; row < 3; row++) {
                    r[0][row] =  sharc_bits_to_float(args[row*4 + 0]);
                    r[1][row] =  sharc_bits_to_float(args[row*4 + 1]);
                    r[2][row] = (row < 2) ? -sharc_bits_to_float(args[row*4 + 2])
                                          :  sharc_bits_to_float(args[row*4 + 2]);
                    g_sharc.pos[row] = sharc_bits_to_float(args[row*4 + 3]);
                }
                g_sharc.matrix_dirty = true;
                g_sharc.bone_dirty   = true;
            }
            return;
        }

        /* ---- Math: 2-in / 1-out ---- */
        case 0x12002424:  /* sin(angle_i16) * float */
            g_sharc.ip_sin_scale = g_last_store_ip;
            if (n >= 2) {
                int16_t ang16 = (int16_t)(args[0] & 0xFFFF);
                float   rad   = ((float)ang16 / 65536.0f) * (2.0f * 3.14159265358979f);
                float   r     = sinf(rad) * sharc_bits_to_float(args[1]);
                SANITIZE(r);
                sharc_push_f(r);
            }
            return;
        case 0x12802525:  /* cos(angle_i16) * float */
            g_sharc.ip_cos_scale = g_last_store_ip;
            if (n >= 2) {
                int16_t ang16 = (int16_t)(args[0] & 0xFFFF);
                float   rad   = ((float)ang16 / 65536.0f) * (2.0f * 3.14159265358979f);
                float   r     = cosf(rad) * sharc_bits_to_float(args[1]);
                SANITIZE(r);
                sharc_push_f(r);
            }
            return;

        /* 0x19003232: f-curve interpolation (get_fcurve_value_f).
         * Args: (t_span, t, v0, v1, tan_out, v_next).
         * Cubic Hermite: u=t/span, m0=tan_out*span/30, m1=0 (zero in-tangent at v1).
         * Verified 55/62 exact matches against MAME fvipers. */
        case 0x19003232:
            if (n >= 6) {
                float span    = sharc_bits_to_float(args[0]);
                float t       = sharc_bits_to_float(args[1]);
                float v0      = sharc_bits_to_float(args[2]);
                float v1      = sharc_bits_to_float(args[3]);
                float tan_out = sharc_bits_to_float(args[4]);
                float r;
                if (span > 0.0f) {
                    float u  = t / span;
                    float u2 = u * u;
                    float u3 = u2 * u;
                    float m0 = tan_out * (span / 30.0f);
                    r = (2.0f*u3 - 3.0f*u2 + 1.0f) * v0
                      + (u3 - 2.0f*u2 + u)         * m0
                      + (-2.0f*u3 + 3.0f*u2)        * v1;
                } else {
                    r = v0;
                }
                SANITIZE(r);
                sharc_push_f(r);
            }
            return;

        case 0x2E005C5C:  /* add_vec3_pairs: (a0,b0, a1,b1, a2,b2) → (a0+b0, ...) */
            if (n >= 6) {
                sharc_push_f(sharc_bits_to_float(args[0]) + sharc_bits_to_float(args[1]));
                sharc_push_f(sharc_bits_to_float(args[2]) + sharc_bits_to_float(args[3]));
                sharc_push_f(sharc_bits_to_float(args[4]) + sharc_bits_to_float(args[5]));
            }
            return;

        case 0x3C007878:  /* polygon submission — MAME verified 9/9 */
            if (n >= 8) {
                uint32_t poly_in    = args[7];
                uint32_t poly_count = args[5] & 0xFFFF;
                sharc_push_u(poly_in);
                sharc_push_u(poly_in + poly_count);
            } else {
                LOG_WARN("SHARC 3C007878: n=%d < 8, no push!", n);
            }
            return;

        case 0x09801313: case 0x0A001414: case 0x0A801515:
        case 0x0B001616: case 0x16802D2D: case 0x13802727:
            if (cmd == 0x13802727) g_sharc.ip_atan2 = g_last_store_ip;
            if (n >= 2) {
                float a = sharc_bits_to_float(args[0]);
                float b = sharc_bits_to_float(args[1]);
                if (cmd == 0x13802727) {
                    float ang = atan2f(b, a);
                    uint32_t ang_u = (uint32_t)(int32_t)(ang * (65536.0f / (2.0f * 3.14159265f)));
                    sharc_push_u(ang_u);
                    return;
                }
                float r;
                switch (cmd) {
                    case 0x09801313: r = a + b; break;
                    case 0x0A001414: r = a - b; break;
                    case 0x0A801515: r = a * b; break;
                    case 0x0B001616: r = (b != 0.0f) ? (a / b) : 0.0f; break;
                    default:         r = sqrtf(a*a + b*b); break;  /* 0x16802D2D */
                }
                SANITIZE(r);
                sharc_push_f(r);
            }
            return;

        /* ---- Math: 1-in / 1-out ---- */
        case 0x10802121:  /* sin(i16 fixed-pt angle) → float  MAME-verified vs sub_7800 */
            if (n >= 1) { float _r = sinf(sharc_angle_to_rad(args[0])); sharc_push_f(_r); }
            return;
        case 0x11002222:  /* cos(i16 fixed-pt angle) → float  MAME-verified vs sub_7800 */
            if (n >= 1) { float _r = cosf(sharc_angle_to_rad(args[0])); sharc_push_f(_r); }
            return;
        case 0x0B801717:
            /* FV PM 0x02059D: F0 = FLOAT R1 — converts integer arg to float. */
            if (n >= 1) { float _r = (float)(int32_t)args[0]; sharc_push_f(_r); }
            return;
        case 0x0C001818:
            if (n >= 1) sharc_push_u(args[0]);
            else        sharc_push_u(0);
            return;

        case 0x1A003434:
            sharc_push_u(0);
            return;

        /* ---- Spatial: 4-in / 1-out ---- */
        case 0x15802B2B:  /* horizontal distance sqrt((x2-x1)²+(z2-z1)²) */
            if (n >= 4) {
                float x1 = sharc_bits_to_float(args[0]), x2 = sharc_bits_to_float(args[1]);
                float z1 = sharc_bits_to_float(args[2]), z2 = sharc_bits_to_float(args[3]);
                float dx = x2-x1, dz = z2-z1;
                float _r = sqrtf(dx*dx + dz*dz);
                sharc_push_f(_r);
            }
            return;
        case 0x17802F2F:  /* azimuth atan2(-(x2-x1), z2-z1) → int16 fixed */
            if (n >= 4) {
                float z1 = sharc_bits_to_float(args[0]), z2 = sharc_bits_to_float(args[1]);
                float x2 = sharc_bits_to_float(args[2]), x1 = sharc_bits_to_float(args[3]);
                float ang = atan2f(-(x2-x1), z2-z1);
                uint32_t ang_u = (uint32_t)(int32_t)(ang * (65536.0f / (2.0f * 3.14159265f)));
                sharc_push_u(ang_u);
            }
            return;
        case 0x2F005E5E:  /* scale_vec3: (scale, x, y, z) → (s*x, s*y, s*z) */
            if (n >= 4) {
                float s = sharc_bits_to_float(args[0]);
                sharc_push_f(s * sharc_bits_to_float(args[1]));
                sharc_push_f(s * sharc_bits_to_float(args[2]));
                sharc_push_f(s * sharc_bits_to_float(args[3]));
            }
            return;

        /* ---- Vector transform: vec3 × matrix ---- */
        case 0x14802929:
        case 0x35006A6A:
            if (cmd == 0x14802929) g_sharc.ip_rot_transform  = g_last_store_ip;
            else                   g_sharc.ip_full_transform = g_last_store_ip;
            if (n >= 3) {
                float ix = sharc_bits_to_float(args[0]);
                float iy = sharc_bits_to_float(args[1]);
                float iz = sharc_bits_to_float(args[2]);
                float ox, oy, oz;
                if (g_sharc.matrix_dirty) sharc_build_matrix();
                { float (*m)[4] = g_sharc.matrix;
                  if (cmd == 0x14802929) {
                      /* FV PM 0x02062F: output = M_rot * v_lh + T.
                       * MAME-verified: −iz for rows 0/1, +iz for row 2. */
                      float niz = -iz;
                      ox = m[0][0]*ix + m[0][1]*iy + m[0][2]*niz + m[0][3];
                      oy = m[1][0]*ix + m[1][1]*iy + m[1][2]*niz + m[1][3];
                      oz = m[2][0]*ix + m[2][1]*iy + m[2][2]*iz  + m[2][3];
                  } else {
                      /* 0x35006A6A world->model = R^T*(v-T) — the true INVERSE of our
                       * verified model->world (0x14802929 = R*v+T, with R[i][j]=rot[j][i];
                       * the matrix-build z-neg and the -iz input cancel to a clean R).
                       * So the inverse contracts by rot COLUMN with NO z-negation.
                       * Used by snc_eye_thd_set (head/eye look-at): camera -> head-bone
                       * local frame -> atan2 look angles. The old code used the SAME
                       * row contraction as model->world (R*(v-T)) = not the inverse →
                       * pose-dependent head rotation error.
                       * EXPERIMENT: branch fix/world2model-transpose. */
                      float rx = ix - g_sharc.pos[0];
                      float ry = iy - g_sharc.pos[1];
                      float rz = iz - g_sharc.pos[2];
                      float (*r)[3] = g_sharc.rot;
                      ox = r[0][0]*rx + r[0][1]*ry + r[0][2]*rz;
                      oy = r[1][0]*rx + r[1][1]*ry + r[1][2]*rz;
                      oz = r[2][0]*rx + r[2][1]*ry + r[2][2]*rz;
                  }
                }
                sharc_push_f(ox); sharc_push_f(oy); sharc_push_f(oz);
                g_sharc.transform_count++;
                g_sharc.dbg_xform_pos[0] = g_sharc.pos[0];
                g_sharc.dbg_xform_pos[1] = g_sharc.pos[1];
                g_sharc.dbg_xform_pos[2] = g_sharc.pos[2];
                g_sharc.dbg_xform_ang[0] = g_sharc.ang[0];
                g_sharc.dbg_xform_ang[1] = g_sharc.ang[1];
                g_sharc.dbg_xform_ang[2] = g_sharc.ang[2];
                g_sharc.dbg_xform_in[0]  = ix; g_sharc.dbg_xform_in[1]  = iy; g_sharc.dbg_xform_in[2]  = iz;
                g_sharc.dbg_xform_out[0] = ox; g_sharc.dbg_xform_out[1] = oy; g_sharc.dbg_xform_out[2] = oz;
            }
            return;

        /* ---- Afterimage/zanzou slot reads ---- */
        case 0x42808585:
            sharc_push_u(0); sharc_push_u(0); sharc_push_u(0);
            sharc_push_u(0); sharc_push_u(0);
            return;
        case 0x42008484:
            return;

        /* ---- Bone matrix cache (SHARC DM[0x30420..0x305A0]) ---- */

        /* 0x1B003636: plain load — PM 0x0204C2.
         * Copies bone slot FROM rot_cache INTO current rot[]/pos[]. */
        case 0x1B003636: {
            if (n < 2) return;
            int player   = (args[0] & 0xFF) == 1 ? 1 : 0;
            int slot_idx = (int)args[1] / 12;
            if ((unsigned)slot_idx >= 16u) return;
            const float *B = g_sharc.rot_cache[player * 16 + slot_idx];
            float (*r)[3] = g_sharc.rot;
            for (int col = 0; col < 3; col++)
                for (int row = 0; row < 3; row++)
                    r[col][row] = B[col * 3 + row];
            g_sharc.pos[0] = B[9];
            g_sharc.pos[1] = B[10];
            g_sharc.pos[2] = B[11];
            g_sharc.matrix_dirty = true;
            g_sharc.bone_dirty   = true;
            return;
        }

        /* 0x1B803737: load + C×B multiply — PM 0x0204D6.
         * Loads bone from rot_cache, computes result = current × bone (affine 3×4
         * product: rot[j][i] = Σ_k B[j*3+k]*r[k][i], pos[i] += Σ_k B[9+k]*r[k][i])
         * where B=bone (rot_cache entry), r=current rotation — current is the left factor.
         * Firmware PM 0x201E9 computes C×B; our indexing matches that convention. */
        case 0x1B803737: {
            if (n < 2) return;
            int player   = (args[0] & 0xFF) == 1 ? 1 : 0;
            int slot_idx = (int)args[1] / 12;
            if ((unsigned)slot_idx >= 16u) return;
            const float *B = g_sharc.rot_cache[player * 16 + slot_idx];
            float (*r)[3] = g_sharc.rot;
            float nr[3][3], np[3];
            for (int j = 0; j < 3; j++)
                for (int i = 0; i < 3; i++) {
                    float s = 0.0f;
                    for (int k = 0; k < 3; k++) s += B[j*3+k] * r[k][i];
                    nr[j][i] = s;
                }
            for (int i = 0; i < 3; i++) {
                float s = g_sharc.pos[i];
                for (int k = 0; k < 3; k++) s += B[9+k] * r[k][i];
                np[i] = s;
            }
            for (int j = 0; j < 3; j++)
                for (int i = 0; i < 3; i++) r[j][i] = nr[j][i];
            g_sharc.pos[0] = np[0];
            g_sharc.pos[1] = np[1];
            g_sharc.pos[2] = np[2];
            g_sharc.matrix_dirty = true;
            g_sharc.bone_dirty   = true;
            /* Mirror post-compose result to tgp_bone so the geo3d scanner reads
             * the C×B world-space matrix when 0x3C007878 follows this command. */
            if ((unsigned)(player * 16 + slot_idx) < 32u) {
                float *tb = g_sharc.tgp_bone[player * 16 + slot_idx];
                for (int _c = 0; _c < 3; _c++)
                    for (int _r = 0; _r < 3; _r++)
                        tb[_c*3+_r] = r[_c][_r];
                tb[9]  = np[0];
                tb[10] = np[1];
                tb[11] = np[2];
            }
            return;
        }

        /* 0x1C803939: set_bone_vec — PM 0x020DC2.
         * Reads 3 float args (x,y,z) in local bone space, transforms to world space
         * using current rot[]/pos[] (raw, no Z-negation — mirrors SHARC subroutine
         * 0x020173 which uses slot[0..11] directly without col2 sign-flip).
         * arg4 = slot_off: DM word offset from coli_buf_base (= bone_slot * 3).
         * Writes 3 world-space floats to BUFF_RAM at coli_buf_base + slot_off*4,
         * and again at coli_buf_base + (slot_off + 0x60)*4 (double-buffer). */
        case 0x1C803939: {
            if (n >= 4 && g_sharc.sharc_dm_ext) {
                float ix = sharc_bits_to_float(args[0]);
                float iy = sharc_bits_to_float(args[1]);
                float iz = sharc_bits_to_float(args[2]);
                uint32_t slot_off = args[3];
                float (*r)[3] = g_sharc.rot;
                /* Raw transform: world = rot * local + pos (no Z-negation) */
                float ox = g_sharc.pos[0] + r[0][0]*ix + r[1][0]*iy + r[2][0]*iz;
                float oy = g_sharc.pos[1] + r[0][1]*ix + r[1][1]*iy + r[2][1]*iz;
                float oz = g_sharc.pos[2] + r[0][2]*ix + r[1][2]*iy + r[2][2]*iz;
                uint32_t base = g_sharc.coli_buf_base;
                uint32_t off1 = base + slot_off * 4;
                uint32_t off2 = base + (slot_off + 0x60u) * 4;
                if (off1 + 12 <= g_sharc.sharc_dm_ext_size) {
                    memcpy(g_sharc.sharc_dm_ext + off1 + 0, &ox, 4);
                    memcpy(g_sharc.sharc_dm_ext + off1 + 4, &oy, 4);
                    memcpy(g_sharc.sharc_dm_ext + off1 + 8, &oz, 4);
                }
                if (off2 + 12 <= g_sharc.sharc_dm_ext_size) {
                    memcpy(g_sharc.sharc_dm_ext + off2 + 0, &ox, 4);
                    memcpy(g_sharc.sharc_dm_ext + off2 + 4, &oy, 4);
                    memcpy(g_sharc.sharc_dm_ext + off2 + 8, &oz, 4);
                }
            }
            return;
        }

        /* 0x24804949: COP internal write — PM 0x02058D. 2 args, no output. */
        case 0x24804949:

        /* 0x40808181: COP internal — PM 0x0208E1. 0 args, no output. */
        case 0x40808181:
        /* 0x43008686: COP internal — PM 0x020955. 1 arg, no output. */
        case 0x43008686:
            return;

        /* 0x22004444: save_bone_to_PM_scratch — PM 0x02054C.
         * 1 arg: N. Copy current 12-word bone (rot+pos, col-major) to pm_bone[N].
         * cpres1 PM 0x02054C: r3=N*12; i8=0x21F20+r3; lcntr=12; dm(i7,m1)->pm(i8,m9). */
        case 0x22004444: {
            if (n >= 1) {
                int _s = (int)(args[0]) & 0xF;
                float (*r)[3] = g_sharc.rot; float *pm = g_sharc.pm_bone[_s];
                pm[0]=r[0][0]; pm[1]=r[0][1]; pm[2]=r[0][2];
                pm[3]=r[1][0]; pm[4]=r[1][1]; pm[5]=r[1][2];
                pm[6]=r[2][0]; pm[7]=r[2][1]; pm[8]=r[2][2];
                pm[9]=g_sharc.pos[0]; pm[10]=g_sharc.pos[1]; pm[11]=g_sharc.pos[2];
            }
            return;
        }

        /* 0x2A005454: smooth_int — STF PM 0x021121 / FV PM 0x020EC7.
         * Applies 9 ang ops (Z,Y,X,X,Y,Z,Y,Z,X) to identity, outputs col0 as 3 × int16 Q14
         * (1.0 = 0x4000) packed in lower 16 bits of 32-bit FIFO words.
         * i960 reads these with ldis (16-bit signed) and uses arithmetic right shifts to
         * compute bone correction deltas stored at 0x690(g7)+bone*12. */
        /* 0x2A805555: dispatch[0x55] — PM 0x02115F. 9 args (3 triples) in, 3 out.
         * The i960 sends 0x2A805555 + 3 stt triples then reads 3 results back (e.g.
         * the 12-iteration loop @0x2FB00). Treating it as 0-arg desynced the input
         * FIFO: every following data word (the triples) was misread as an opcode →
         * the 0xFFFF.../0.5f "unknown cmd" WARN spam → corrupted ALL downstream COP
         * state, including the stage/cage matrix (cage/pole drift cascade).
         * Reads return zero (MAME), so consume the 9 args and emit 3 zero outputs. */
        case 0x2A805555:
            sharc_push_u(0); sharc_push_u(0); sharc_push_u(0);
            return;

        /* 0x08001010: dispatch[0x10] — PM 0x020460. 0 args, 0 outputs.
         * IDA kira_kira_disp: written with no args before ang_z; pure state op. */
        case 0x08001010:
            return;

        /* 0x34006868: dispatch[0x68] — PM 0x0205A3. 1 arg, 0 outputs.
         * IDA name_char_kage_disp: lda 0x3D00 as sole arg; kage/shadow bone init. */
        case 0x34006868:
            return;

        /* 0x3B807777: sphere_coli_check — dispatch[0x77] PM 0x020B1F.
         * IDA epc_oidasi: 4 args (x, y, z, flags); 9 outputs.
         * Outputs[0..1] = world-space position correction deltas (scaled by 0x3E23D70A).
         * Outputs[2..8] = read and discarded by i960 caller.
         * HLE: return 9 zeros -> no sphere collision correction applied. */
        case 0x3B807777: {
            int _i; for (_i = 0; _i < 9; _i++) sharc_push_u(0);
            return;
        }

        /* 0x2D005A5A: dispatch[0x5A] — PM 0x0206F4. 2 args, 2 outputs.
         * IDA os_set_coli: args (col0.x, col0.y); 2 reads back into g4, g5.
         * With identity rotation the outputs equal the inputs; treat as passthrough. */
        case 0x2D005A5A:
            sharc_push_f(n > 0 ? sharc_bits_to_float(args[0]) : 0.0f);
            sharc_push_f(n > 1 ? sharc_bits_to_float(args[1]) : 0.0f);
            return;

        case 0x2A005454: {
            if (n < 9) { sharc_push_u(0); sharc_push_u(0); sharc_push_u(0); return; }
            /* Firmware PM 0x21121: call set_identity, then apply 9 ang ops, then
             * jump to _L2117C which extracts Euler angles from the result matrix.
             * Axis sequence (from disassembly): X,Y,Z,Z,Y,X,Y,X,Z
             * (was incorrectly Z,Y,X,X,Y,Z,Y,Z,X) */
            float r[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
            static const int8_t si_axes[9] = {0,1,2,2,1,0,1,0,2};
            int si_i, si_row;
            for (si_i = 0; si_i < 9; si_i++) {
                float si_a = sharc_angle_to_rad((int32_t)args[si_i]);
                float si_c = cosf(si_a), si_s = sinf(si_a);
                int8_t si_ax = si_axes[si_i];
                if (si_ax == 1) {       /* ang_y: col0,col2 */
                    for (si_row=0;si_row<3;si_row++){float t=r[0][si_row];r[0][si_row]=si_c*t+si_s*r[2][si_row];r[2][si_row]=-si_s*t+si_c*r[2][si_row];}
                } else if (si_ax == 0){ /* ang_x: col1,col2 */
                    for (si_row=0;si_row<3;si_row++){float t=r[1][si_row];r[1][si_row]=si_c*t-si_s*r[2][si_row];r[2][si_row]=si_s*t+si_c*r[2][si_row];}
                } else {                /* ang_z: col0,col1 */
                    for (si_row=0;si_row<3;si_row++){float t=r[0][si_row];r[0][si_row]=si_c*t-si_s*r[1][si_row];r[1][si_row]=si_s*t+si_c*r[1][si_row];}
                }
            }
            /* _L2117C: extract three Euler-like angles from result matrix.
             *   a0 = atan2(col2[row0], col2[row2])  = atan2(r[2][0], r[2][2])
             *   a1 = acos(col2[row1])               = acos(r[2][1])
             *   a2 = atan2(col0[row1], col1[row1])  = atan2(r[0][1], r[1][1])
             * Then apply π-wrap optimization: for each angle, compare |original| vs
             * |adjusted| (adj0=a0±π, adj1=π-a1, adj2=a2±π); if sum(|adj|)<sum(|orig|)
             * use adjusted set. Output as i16 fixed-point (scale=32768/π). */
            { float pi = 3.14159265358979f;
              float sc = 32768.0f / pi;
              float a0 = atan2f(r[2][0], r[2][2]);
              float a1 = acosf(r[2][1] < -1.0f ? -1.0f : r[2][1] > 1.0f ? 1.0f : r[2][1]);
              float a2 = atan2f(r[0][1], r[1][1]);
              /* adjusted candidates */
              float adj0 = (a0 < 0.0f) ? (a0 + pi) : (a0 - pi);
              float adj1 = pi - a1;   /* acos always in [0,π], adjusted is π-a1 */
              float adj2 = (a2 < 0.0f) ? (a2 + pi) : (a2 - pi);
              /* pick whichever set has smaller total absolute value */
              if (fabsf(adj0)+fabsf(adj1)+fabsf(adj2) < fabsf(a0)+fabsf(a1)+fabsf(a2)) {
                  a0 = adj0; a1 = adj1; a2 = adj2;
              }
              sharc_push_u((uint32_t)(uint16_t)(int16_t)(int)(a0 * sc));
              sharc_push_u((uint32_t)(uint16_t)(int16_t)(int)(a1 * sc));
              sharc_push_u((uint32_t)(uint16_t)(int16_t)(int)(a2 * sc));
            }
            return;
        }

        /* 0x1E803D3D: add_pos_delta — PM 0x0204F2.
         * 6 float args as two (x,y,z) triplets. Reads a 16-element float array at
         * DM[0x30429], accumulates arg1..3 into it, and similarly arg4..6 into the
         * parallel array at DM[0x304E9]. Used for collision geometry offsets. */
        case 0x1E803D3D:

        /* 0x1D003A3A: init_coll_slot — PM 0x020DDF.
         * 4 args: stores arg1 → DM[I3+0x11], arg2 → DM[I3+0x12],
         * arg3 → DM[0x3041A], arg4 → DM[0x3041B]. Clears DM[I3+0x10]=0,
         * fills 32 words at DM[0x01403E20] with 0xFFFFFFFF, sets DM[I3+5]=1. */
        case 0x1D003A3A:

        /* 0x1F003E3E: COP collision — PM 0x0210B2. 5 args, no output. */
        case 0x1F003E3E:
            return;
        /* 0x1D803B3B: collision_setup_v2 — PM 0x020ED5.
         * 7 args, no FIFO output. Sets up streaming animation collision data buffers. */
        case 0x1D803B3B:
            return;
        case 0x39007272:
            sharc_push_u(0);
            return;
        /* 0x38007070: init_coli_sphere_sys — PM 0x020BBE.
         * 5 args: (r15, r13, r14, r11, r12) set up sphere collision tables for a player.
         * cpres1 _L20BBA return path: no dm(m0,i1) writes -> 0 FIFO outputs. */
        case 0x38007070:
            /* PM 0x20BBE: stage-object CLIP/BOUNDS classifier. Reads 5 args (stage
             * bounds), classifies up to 32 objects (positions DM[0x1403e80]/[0x1407e80],
             * vis DM[0x30600]/[0x30700]) against the bounds, outputs 22 words → i960
             * stores to g13+0x118.. and g7+0x650/0x660/0xA58/0x614..
             * STUB for now: a faithful port needs the SHARC DM object table our HLE
             * doesn't model. MAME ground-truth (one fight frame, for the eventual port):
             *   g13+118=0x33000 g13+124=0x33000  (clip masks)
             *   g7+710=2.56  g7+A5C=7.30
             *   g7+650..65C = 8.70,5.47,8.66,5.83  (arena bounds; init 100.0 minimized)
             *   g7+614=0x9000 g7+618=0x9000        (remapped masks)
             *   all other dests = 0
             * The empty-set default (0s+100.0) was verified WRONG vs MAME, reverted. */
            return;

        case 0x24004848:
            sharc_push_u(0);
            return;

        case 0x2B005656:
            /* Read col0 of current matrix (first 3 words in SHARC column-major layout).
             * In row-major terms: [m[0][0], m[1][0], m[2][0]]. */
            if (g_sharc.matrix_dirty) sharc_build_matrix();
            sharc_push_f(g_sharc.matrix[0][0]);
            sharc_push_f(g_sharc.matrix[1][0]);
            sharc_push_f(g_sharc.matrix[2][0]);
            return;

        /* 0x2D805B5B: passthrough_yz — PM 0x020700.
         * 3 args (entity_idx, y, z) -> 2 floats: (y, z).
         * MAME-verified: stride-2 stall pattern; real outputs = args[1], args[2].
         * When entity_idx == 0 the outputs equal the inputs unchanged. */
        case 0x2D805B5B:
            sharc_push_f(n > 1 ? sharc_bits_to_float(args[1]) : 0.0f);
            sharc_push_f(n > 2 ? sharc_bits_to_float(args[2]) : 0.0f);
            return;
        /* 0x2C805959: dot2D — PM 0x02068C.
         * 4 args (a,b,c,d) -> 1 float: a*b + c*d.
         * cpres1 PM 0x02068C: f8=f0*f4+f1*f5 where (f0,f1)=(arg0,arg2), (f4,f5)=(arg1,arg3). */
        case 0x2C805959: {
            float _a = (n > 0) ? sharc_bits_to_float(args[0]) : 0.0f;
            float _b = (n > 1) ? sharc_bits_to_float(args[1]) : 0.0f;
            float _c = (n > 2) ? sharc_bits_to_float(args[2]) : 0.0f;
            float _d = (n > 3) ? sharc_bits_to_float(args[3]) : 0.0f;
            sharc_push_f(_a*_b + _c*_d);
            return;
        }

        /* 0x3F807F7F: flush_bone_sel — STF PM 0x020DB3.
         * Copies 192 words from selection buffer to output buffer. No FIFO output.
         * NOTE: FV firmware maps index 0x7F to the error/hang handler — this command
         * is STF-specific and must not be sent by FV game code. */
        case 0x3F807F7F:

        /* 0x1C003838: select_bone_buf — PM 0x020DA8.
         * Writes base pointer to DM[0x3033E]: 0x01403E80 (arg≠1) or 0x01407E80 (arg=1).
         * P1 buf → BUFF_RAM+0x00FA00, P2 buf → BUFF_RAM+0x01FA00.
         * Selects active player's bone animation data buffer. No FIFO output. */
        case 0x1C003838:
            g_sharc.coli_buf_base = (n >= 1 && (args[0] & 0xFF) == 1) ? 0x1FA00u : 0xFA00u;
            return;

        case 0x09001212:
            /* Output 9 raw slot values in column-major order, no z-negation.
             * Firmware (PM 0x02045B, fvipers): LCNTR=9; R0=DM(I7,M1); DM(M0,I1)=R0.
             * Slot layout: slot[col*3+row] = rot[col][row].
             * Output order: rot[0][0..2], rot[1][0..2], rot[2][0..2]. */
            for (int _col = 0; _col < 3; _col++)
                for (int _row = 0; _row < 3; _row++)
                    sharc_push_f(g_sharc.rot[_col][_row]);
            return;

        /* 0x35806B6B: calc_rob_angle_cont — 2-bone IK chain transform.
         *
         * SHARC-verified algorithm (553-step single-step trace):
         *   args[0..2]  = skel_offset (a0,a1,a2): local bone offset in parent space
         *   args[3..8]  = 6 joint angles (i16 fixed-pt): Rz,Ry,Rx,Ry,Rx,Rz order
         *   args[9..11] = target pos (a9,a10,a11): world-space IK target
         *   args[12]    = a12: lower bone length — forearm or shin. Named by a
         *                 MAME capture of a real fight: stf-tools/motion-pose.csv
         *                 holds args[12] against the character record's forearm
         *                 slot and args[13] against its upper arm, and the two
         *                 differ (0.3932 against 0.3464 for a thigh and shin),
         *                 so the pairing is not a coin toss.
         *   args[13]    = a13: upper bone length — upper arm or thigh. This is
         *                 the bone that hangs at the pivot; a12 carries on from
         *                 the elbow it reaches.
         *   args[14]    = TGP word address for the LOWER bone's matrix
         *                 (left arm: 0x3A30, slot 4, the forearm)
         *   args[15]    = TGP word address for the UPPER bone's matrix
         *                 (left arm: 0x3A24, slot 3, the upper arm)
         *   args[16]    = flip flag: 0 = elbow below, non-zero = elbow above (negates sin_sh/sin_el) */
        case 0x35806B6B: {
            if (n < 17) { sharc_push_u(0); return; }

            float a0  = sharc_bits_to_float(args[0]);
            float a1  = sharc_bits_to_float(args[1]);
            float a2  = sharc_bits_to_float(args[2]);
            int32_t ang3 = (int32_t)args[3];
            int32_t ang4 = (int32_t)args[4];
            int32_t ang5 = (int32_t)args[5];
            int32_t ang6 = (int32_t)args[6];
            int32_t ang7 = (int32_t)args[7];
            int32_t ang8 = (int32_t)args[8];
            float a9  = sharc_bits_to_float(args[9]);
            float a10 = sharc_bits_to_float(args[10]);
            float a11 = sharc_bits_to_float(args[11]);
            float a12 = sharc_bits_to_float(args[12]);
            float a13 = sharc_bits_to_float(args[13]);
            uint32_t slot14 = args[14];
            uint32_t slot15 = args[15];

            if (g_sharc.bone_dirty) {
                /* Initialise bone scratch from raw rot[] (no z-negation — bone math
                 * uses the same column-major convention as the SHARC firmware). */
                float (*r)[3] = g_sharc.rot;
                g_sharc.bone_col[0] = r[0][0]; g_sharc.bone_col[1] = r[0][1]; g_sharc.bone_col[2] = r[0][2];
                g_sharc.bone_col[3] = r[1][0]; g_sharc.bone_col[4] = r[1][1]; g_sharc.bone_col[5] = r[1][2];
                g_sharc.bone_col[6] = r[2][0]; g_sharc.bone_col[7] = r[2][1]; g_sharc.bone_col[8] = r[2][2];
                g_sharc.bone_T[0]   = g_sharc.pos[0];
                g_sharc.bone_T[1]   = g_sharc.pos[1];
                g_sharc.bone_T[2]   = g_sharc.pos[2];
                g_sharc.bone_dirty  = false;
            }

            float *M = g_sharc.bone_col;
            float *T = g_sharc.bone_T;

            T[0] += a0*M[0] + a1*M[3] + a2*M[6];
            T[1] += a0*M[1] + a1*M[4] + a2*M[7];
            T[2] += a0*M[2] + a1*M[5] + a2*M[8];

#define BONE_RZ(ang) do { \
    float _r = sharc_angle_to_rad(ang); float _c = cosf(_r), _s = sinf(_r); \
    int _i; for (_i = 0; _i < 3; _i++) { \
        float _x = M[_i], _y = M[_i+3]; \
        M[_i]   = _c*_x - _s*_y; \
        M[_i+3] = _s*_x + _c*_y; } \
} while(0)
#define BONE_RY(ang) do { \
    float _r = sharc_angle_to_rad(ang); float _c = cosf(_r), _s = sinf(_r); \
    int _i; for (_i = 0; _i < 3; _i++) { \
        float _x = M[_i], _z = M[_i+6]; \
        M[_i]   = _c*_x + _s*_z; \
        M[_i+6] = _c*_z - _s*_x; } \
} while(0)
#define BONE_RX(ang) do { \
    float _r = sharc_angle_to_rad(ang); float _c = cosf(_r), _s = sinf(_r); \
    int _i; for (_i = 0; _i < 3; _i++) { \
        float _y = M[_i+3], _z = M[_i+6]; \
        M[_i+3] = _c*_y - _s*_z; \
        M[_i+6] = _s*_y + _c*_z; } \
} while(0)
            BONE_RZ(ang3); BONE_RY(ang4); BONE_RX(ang5);
            BONE_RY(ang6); BONE_RX(ang7); BONE_RZ(ang8);
#undef BONE_RZ
#undef BONE_RY
#undef BONE_RX

            float dx = a9  - T[0];
            float dy = a10 - T[1];
            float dz = a11 - T[2];

            float d0 = dx*M[0] + dy*M[1] + dz*M[2];
            float d1 = dx*M[3] + dy*M[4] + dz*M[5];
            float d2 = dx*M[6] + dy*M[7] + dz*M[8];

            float d_xy2   = d0*d0 + d1*d1;
            float d2_tot  = d_xy2 + d2*d2;
            float d_xy    = sqrtf(d_xy2);
            float d_total = sqrtf(d2_tot);

#define BCLIP(v) ((v) > 1.0f ? 1.0f : ((v) < -1.0f ? -1.0f : (v)))

            if (d_xy > 1e-7f) {
                float c = BCLIP(d0 / d_xy);
                float s = BCLIP(-d1 / d_xy);
                int _i; for (_i = 0; _i < 3; _i++) {
                    float _x = M[_i], _y = M[_i+3];
                    M[_i]   = c*_x - s*_y;
                    M[_i+3] = s*_x + c*_y;
                }
            }

            if (d_total > 1e-7f) {
                float c = BCLIP(d_xy / d_total);
                float s = BCLIP(d2 / d_total);
                int _i; for (_i = 0; _i < 3; _i++) {
                    float _x = M[_i], _z = M[_i+6];
                    M[_i]   = c*_x + s*_z;
                    M[_i+6] = c*_z - s*_x;
                }
            }

/* Store one solved bone into the TGP slot its address names, and into the
 * SHARC's own data space beside it. The two bones of a chain do not sit in the
 * same place, so the matrix and the translation are both passed in. */
#define WRITE_TGP(tgp_addr, mat, tr) do { \
    uint32_t _a = (tgp_addr); \
    int _idx = (_a >= 0x3B00 && _a < 0x3C00) ? (int)(16 + (_a - 0x3B00) / 0xC) \
             : (_a >= 0x3A00 && _a < 0x3B00) ? (int)(     (_a - 0x3A00) / 0xC) \
             : -1; \
    if (_idx >= 0 && _idx < 32) { \
        float *_d = g_sharc.tgp_bone[_idx]; \
        int _k; for (_k = 0; _k < 9; _k++) _d[_k] = (mat)[_k]; \
        _d[9] = (tr)[0]; _d[10] = (tr)[1]; _d[11] = (tr)[2]; \
        if (g_sharc.sharc_dm_ext) { \
            uint32_t _bo = _a * 4; \
            if (_bo + 48 <= g_sharc.sharc_dm_ext_size) { \
                for (_k = 0; _k < 12; _k++) { \
                    uint32_t _u = sharc_float_to_bits(_d[_k]); \
                    memcpy(g_sharc.sharc_dm_ext + _bo + _k*4, &_u, 4); \
                } \
            } \
        } \
    } \
} while(0)

            /* The chain is two bones and they do not start in the same place.
             * The upper one — upper arm or thigh, a13 — hangs at the pivot; the
             * lower one — forearm or shin, a12 — carries on from the elbow the
             * upper one reaches, which is one upper-bone length along the upper
             * bone's own +X.  args[14] names the lower bone's slot and args[15]
             * the upper's: TGP addresses step 0x0C a slot from 0x3A00, so the
             * left arm's pair is 0x3A30 (slot 4, the forearm) and 0x3A24
             * (slot 3, the upper arm).
             *
             * Both matrices come out of one post-multiply chain — the first
             * turn is the lower bone's frame, the second the upper's — so the
             * lower's has to be kept before the second turn overwrites it.
             *
             * Writing them the other way round still leaves the limb touching
             * its target, because the triangle's two edges add to the same
             * point whichever order they are walked in, and it still bends by
             * the right angle.  What moves is the elbow, to the far corner of
             * that parallelogram: the two bones swap ends, so the thigh is
             * drawn from the knee down and the joint folds backwards.  Which is
             * why this was worth measuring rather than eyeballing —
             * tools/grade-pose.mjs put it at 0.385 world units, one bone
             * length, against the explorer's rig. */
            float M_low[9], T_low[3], T_up[3];
            T_up[0] = T[0]; T_up[1] = T[1]; T_up[2] = T[2];

            if ((a12 + a13) <= d_total) {
                /* Out of reach: the chain gives up bending and lies straight
                 * along the aim, the elbow still an upper bone from the pivot. */
                memcpy(M_low, M, sizeof(M_low));
                T_low[0] = T_up[0] + a13 * M[0];
                T_low[1] = T_up[1] + a13 * M[1];
                T_low[2] = T_up[2] + a13 * M[2];
                WRITE_TGP(slot15, M, T_up);
                WRITE_TGP(slot14, M_low, T_low);
                /* IK reads parent-body scratchpad but does NOT write back to it.
                 * Mark dirty so the next arm reinitializes from g_sharc.rot/pos. */
                g_sharc.bone_dirty = true;
                sharc_push_u(0); return;
            }

            {
                int flip = (args[16] != 0);
                float cos_sh = (a12*a12 + d2_tot - a13*a13) / (2.0f * a12 * d_total);
                cos_sh = BCLIP(cos_sh);
                float sin_sh = -sqrtf(1.0f - cos_sh*cos_sh);
                if (flip) sin_sh = -sin_sh;
                sin_sh = BCLIP(sin_sh);
                int _i; for (_i = 0; _i < 3; _i++) {
                    float _x = M[_i], _y = M[_i+3];
                    M[_i]   = cos_sh*_x - sin_sh*_y;
                    M[_i+3] = sin_sh*_x + cos_sh*_y;
                }
            }

            /* The lower bone's frame, before the elbow turn is folded in. */
            memcpy(M_low, M, sizeof(M_low));

            {
                int flip = (args[16] != 0);
                float cos_el = (a12*a12 + a13*a13 - d2_tot) / (2.0f * a12 * a13);
                float neg_cos = BCLIP(-cos_el);
                float sin_el  = BCLIP(sqrtf(1.0f - cos_el*cos_el));
                if (flip) sin_el = -sin_el;
                int _i; for (_i = 0; _i < 3; _i++) {
                    float _x = M[_i], _y = M[_i+3];
                    M[_i]   = neg_cos*_x - sin_el*_y;
                    M[_i+3] = sin_el*_x  + neg_cos*_y;
                }
            }

            /* M is the upper bone now; the elbow is one upper bone along it. */
            T_low[0] = T_up[0] + a13 * M[0];
            T_low[1] = T_up[1] + a13 * M[1];
            T_low[2] = T_up[2] + a13 * M[2];

            WRITE_TGP(slot15, M, T_up);
            WRITE_TGP(slot14, M_low, T_low);

            /* IK reads parent-body scratchpad but does NOT write back to it.
             * Mark dirty so the next arm reinitializes from g_sharc.rot/pos. */
            g_sharc.bone_dirty = true;

            sharc_push_u(0);
#undef BCLIP
#undef WRITE_TGP
            return;
        }

        /* 0x31806363: get_frame_dat — 7 args, 3 results.
         * The cpres1 firmware at PM 0x21209 computes a relative-position-in-rotated-
         * frame: out=[(E-A)cosD+(G-C)sinD, F-B, (G-C)cosD-(E-A)sinD] for args
         * [A,B,C,D,E,F,G].  But MAME-verified: STF's real COP returns 0,0,0 even for
         * args [0,0,0,0,0,6,6] (where cpres1 would give 0,6,6), so cpres1 diverges
         * from STF here.  Until STF's true semantics are captured from MAME, return
         * zeros — which matches MAME for all observed inputs. */
        case 0x31806363:
            sharc_push_u(0); sharc_push_u(0); sharc_push_u(0);
            return;

        /* 0x08801111: load 3×3 rotation into current slot — PM 0x020443.
         * Firmware: LCNTR=9; R0=DM(M0,I0); DM(I7,M1)=R0 (I7=DM[0x3033F]).
         * Slot layout is col-major: slot[col*3+row] = rot[col][row].
         * Sets the base rotation; subsequent ang commands post-multiply from here. */
        case 0x08801111:
            if (n >= 9) {
                for (int _col = 0; _col < 3; _col++)
                    for (int _row = 0; _row < 3; _row++)
                        g_sharc.rot[_col][_row] = sharc_bits_to_float(args[_col*3 + _row]);
                g_sharc.matrix_dirty = true;
                g_sharc.bone_dirty   = true;
            }
            return;

        case 0x41008282:
            sharc_push_u(0);
            return;

        /* 0x18003030: rotate2D_passz  dispatch index 0x30  PM 0x206E3
         * args: (angle_i16, x, y) → (x*cos - y*sin, x*sin + y*cos, y unchanged)
         * Firmware (cpres1 PM 0x206E3): call _L202C1(angle) → cos/sin,
         *   f8=cos*x-sin*y, f9=sin*x+cos*y → output f8,f9.
         * STF firmware outputs 3 values (i960 reads 3 with ldt); 3rd = z passthrough. */
        case 0x18003030:
            if (n >= 3) {
                float _ang = sharc_angle_to_rad((int32_t)args[0]);
                float _c = cosf(_ang), _s = sinf(_ang);
                float _x = sharc_bits_to_float(args[1]);
                float _y = sharc_bits_to_float(args[2]);
                sharc_push_f(_c*_x - _s*_y);
                sharc_push_f(_s*_x + _c*_y);
                sharc_push_u(args[2]);  /* z passthrough */
            } else {
                sharc_push_u(0); sharc_push_u(0); sharc_push_u(0);
            }
            return;

        /* 0x13002626: asin(a) -> i16_angle  (dispatch index 0x26, PM 0x20636)
         * Firmware: _L20332 calls sqrt(1-f1^2) + atan2(f1,sqrt) -> i16.
         * i960 passes a float, reads back 16-bit angle (via ldis).
         * IDA-confirmed: 9 call sites, result compared against angle thresholds. */
        case 0x13002626:
            if (n >= 1) {
                float _a = sharc_bits_to_float(args[0]);
                if (_a >  1.0f) _a =  1.0f;
                if (_a < -1.0f) _a = -1.0f;
                float _ang = asinf(_a);
                uint32_t _u = (uint32_t)(int32_t)(_ang * (65536.0f / (2.0f * 3.14159265358979f)));
                sharc_push_u(_u);
            }
            return;

        case 0x1F803F3F:
            /* set_ang_xyz: firmware PM 0x0203DB calls ang_z(arg0), ang_y(arg1), ang_x(arg2).
             * No world_pos snapshot — that only happens in the standalone 0x04800909 wrapper. */
            if (n >= 3) {
                g_sharc.ang[2] = (int32_t)args[0];  /* z angle */
                g_sharc.ang[1] = (int32_t)args[1];  /* y angle */
                g_sharc.ang[0] = (int32_t)args[2];  /* x angle */
                { float a = sharc_angle_to_rad(g_sharc.ang[2]); sharc_postmul_rz(cosf(a), sinf(a)); }
                { float a = sharc_angle_to_rad(g_sharc.ang[1]); sharc_postmul_ry(cosf(a), sinf(a)); }
                { float a = sharc_angle_to_rad(g_sharc.ang[0]); sharc_postmul_rx(cosf(a), sinf(a)); }
                g_sharc.matrix_dirty = true;
                g_sharc.bone_dirty   = true;
            }
            sharc_push_u(0);
            return;

        /* ---- Matrix stack: push / pop ---- */
        case 0x00800101:
            if (g_sharc.stack_top < SHARC_STACK_DEPTH) {
                int sp = g_sharc.stack_top++;
                memcpy(g_sharc.stack[sp].rot,      g_sharc.rot,       sizeof(g_sharc.rot));
                g_sharc.stack[sp].ang[0]       = g_sharc.ang[0];
                g_sharc.stack[sp].ang[1]       = g_sharc.ang[1];
                g_sharc.stack[sp].ang[2]       = g_sharc.ang[2];
                g_sharc.stack[sp].pos[0]       = g_sharc.pos[0];
                g_sharc.stack[sp].pos[1]       = g_sharc.pos[1];
                g_sharc.stack[sp].pos[2]       = g_sharc.pos[2];
                g_sharc.stack[sp].world_pos[0] = g_sharc.world_pos[0];
                g_sharc.stack[sp].world_pos[1] = g_sharc.world_pos[1];
                g_sharc.stack[sp].world_pos[2] = g_sharc.world_pos[2];
            }
            return;
        case 0x01000202:
            if (g_sharc.stack_top > 0) {
                int sp = --g_sharc.stack_top;
                memcpy(g_sharc.rot,      g_sharc.stack[sp].rot,       sizeof(g_sharc.rot));
                g_sharc.ang[0]       = g_sharc.stack[sp].ang[0];
                g_sharc.ang[1]       = g_sharc.stack[sp].ang[1];
                g_sharc.ang[2]       = g_sharc.stack[sp].ang[2];
                g_sharc.pos[0]       = g_sharc.stack[sp].pos[0];
                g_sharc.pos[1]       = g_sharc.stack[sp].pos[1];
                g_sharc.pos[2]       = g_sharc.stack[sp].pos[2];
                g_sharc.world_pos[0] = g_sharc.stack[sp].world_pos[0];
                g_sharc.world_pos[1] = g_sharc.stack[sp].world_pos[1];
                g_sharc.world_pos[2] = g_sharc.stack[sp].world_pos[2];
                g_sharc.matrix_dirty = true;
                g_sharc.bone_dirty   = true;
            }
            return;
        case 0x01800303:  /* load identity: reset rotation and translation */
            sharc_rot_identity();
            g_sharc.ang[0] = g_sharc.ang[1] = g_sharc.ang[2] = 0;
            g_sharc.pos[0] = g_sharc.pos[1] = g_sharc.pos[2] = 0.0f;
            g_sharc.world_pos[0] = g_sharc.world_pos[1] = g_sharc.world_pos[2] = 0.0f;
            g_sharc.matrix_dirty = true;
            g_sharc.bone_dirty   = true;
            return;

        /* ---- Bone slot write/select commands — no FIFO output ---- */

        /* 0x07000E0E: write_bone_vec3 — PM 0x020433.
         * Firmware: advances I7 past slot[0], writes 3 FIFO args to slot[1..3]
         * (rotation entries col0[1..2] and col1[0]).  In the HLE the global rot[]
         * is reset to identity by the preceding 0x01800303 and is written by the
         * subsequent ang commands, so there is nothing to mirror here. */
        case 0x07000E0E:
            return;

        /* 0x1A803535: save current matrix to bone cache — PM 0x0204AE.
         * arg0 & 0xFF selects player: 1 → P2 base (slots 16..31), else P1 (0..15).
         * arg1 = bone_idx * 12 (word offset in the player's bone table).
         * Copies current rot[]/pos[] → rot_cache[player*16 + slot_idx]. */
        case 0x1A803535: {
            if (n < 2) return;
            int player   = (args[0] & 0xFF) == 1 ? 1 : 0;
            int slot_idx = (int)args[1] / 12;
            if ((unsigned)slot_idx >= 16u) return;
            float *dst = g_sharc.rot_cache[player * 16 + slot_idx];
            float (*r)[3] = g_sharc.rot;
            for (int col = 0; col < 3; col++)
                for (int row = 0; row < 3; row++)
                    dst[col * 3 + row] = r[col][row];
            dst[9]  = g_sharc.pos[0];
            dst[10] = g_sharc.pos[1];
            dst[11] = g_sharc.pos[2];
            /* Mirror to tgp_bone so geo3d scanner finds the world-space matrix
             * when 0x1B803737 references this slot.  (tgp_bone is otherwise only
             * written by 0x35806B6B IK chains, which aren't called in attract.) */
            memcpy(g_sharc.tgp_bone[player * 16 + slot_idx], dst, 12 * sizeof(float));
            return;
        }

        /* 0x34806969: load_anim_frame — PM 0x021237.
         * arg0 = word offset into BUFF_RAM (written by 0x33806767 from IK solver).
         * arg1 = character facing angle (world-space Ry, pre-multiplied onto bone).
         * Loads 9 col-major floats as bone base rotation, then applies
         * Ry(arg1) as a world-space pre-multiply: rot = Ry(arg1) × bone_rot. */
        case 0x34806969: {
            if (n >= 1 && g_sharc.sharc_dm_ext) {
                uint32_t byte_off = args[0] * 4;
                if (byte_off + 9 * 4 <= g_sharc.sharc_dm_ext_size) {
                    for (int _col = 0; _col < 3; _col++)
                        for (int _row = 0; _row < 3; _row++) {
                            uint32_t u;
                            memcpy(&u, g_sharc.sharc_dm_ext + byte_off + (_col*3+_row)*4, 4);
                            g_sharc.rot[_col][_row] = sharc_bits_to_float(u);
                        }
                    if (n >= 2) {
                        float a = sharc_angle_to_rad((int32_t)args[1]);
                        sharc_premul_ry(cosf(a), sinf(a));
                    }
                    g_sharc.matrix_dirty = true;
                    g_sharc.bone_dirty   = true;
                }
            }
            return;
        }

        /* 0x31006262: set_body_matrix — PM 0x0211E1.
         * Performs an INTERNAL PUSH, then sets the new bone slot's T = args[0..2]
         * (world position) and builds a 3×3 rotation matrix from args[3..5]
         * (body Euler angles from g7+0x140..0x144), then post-multiplies
         * ang_y(args[6]), ang_x(args[7]), ang_z(args[8]) (world angles from g7+0xC00..0xC04).
         *
         * Firmware derivation (PM 0x0211E1):
         *   CALL(0x020375)                   ← internal push
         *   I7 = DM[0x3033F]
         *   DM[I7+9..11] = args[0..2]        ← world T
         *   cos3,sin3 = sincos(args[3])
         *   cos4,sin4 = sincos(args[4])
         *   cos5,sin5 = sincos(args[5])
         *   PM 0x0211C0: build rot[][]:
         *     rot[0] = [c4*c3, -c4*s3, s4]
         *     rot[1] = [s3*c5+c3*s4*s5, c3*c5-s3*s4*s5, -c4*s5]
         *     rot[2] = [s3*s5-c3*s4*c5, c3*s5+s3*s4*c5,  c4*c5]
         *   ang_y(args[6]); ang_x(args[7]); ang_z(args[8])
         */
        case 0x31006262: {
            if (n < 9) return;
            /* internal push */
            if (g_sharc.stack_top < SHARC_STACK_DEPTH) {
                int _sp = g_sharc.stack_top++;
                memcpy(g_sharc.stack[_sp].rot, g_sharc.rot, sizeof(g_sharc.rot));
                g_sharc.stack[_sp].ang[0]       = g_sharc.ang[0];
                g_sharc.stack[_sp].ang[1]       = g_sharc.ang[1];
                g_sharc.stack[_sp].ang[2]       = g_sharc.ang[2];
                g_sharc.stack[_sp].pos[0]       = g_sharc.pos[0];
                g_sharc.stack[_sp].pos[1]       = g_sharc.pos[1];
                g_sharc.stack[_sp].pos[2]       = g_sharc.pos[2];
                g_sharc.stack[_sp].world_pos[0] = g_sharc.world_pos[0];
                g_sharc.stack[_sp].world_pos[1] = g_sharc.world_pos[1];
                g_sharc.stack[_sp].world_pos[2] = g_sharc.world_pos[2];
            }
            /* set world translation */
            g_sharc.pos[0] = sharc_bits_to_float(args[0]);
            g_sharc.pos[1] = sharc_bits_to_float(args[1]);
            g_sharc.pos[2] = sharc_bits_to_float(args[2]);
            /* build rotation matrix from body angles (args[3..5]) via PM 0x0211C0 */
            { float c3 = cosf(sharc_angle_to_rad((int32_t)args[3]));
              float s3 = sinf(sharc_angle_to_rad((int32_t)args[3]));
              float c4 = cosf(sharc_angle_to_rad((int32_t)args[4]));
              float s4 = sinf(sharc_angle_to_rad((int32_t)args[4]));
              float c5 = cosf(sharc_angle_to_rad((int32_t)args[5]));
              float s5 = sinf(sharc_angle_to_rad((int32_t)args[5]));
              float (*r)[3] = g_sharc.rot;
              r[0][0] =  c4*c3;              r[0][1] = -c4*s3;              r[0][2] =  s4;
              r[1][0] =  s3*c5 + c3*s4*s5;  r[1][1] =  c3*c5 - s3*s4*s5;  r[1][2] = -c4*s5;
              r[2][0] =  s3*s5 - c3*s4*c5;  r[2][1] =  c3*s5 + s3*s4*c5;  r[2][2] =  c4*c5;
            }
            /* post-multiply world angles: ang_y(args[6]), ang_x(args[7]), ang_z(args[8]) */
            { float _a = sharc_angle_to_rad((int32_t)args[6]); sharc_postmul_ry(cosf(_a), sinf(_a)); }
            { float _a = sharc_angle_to_rad((int32_t)args[7]); sharc_postmul_rx(cosf(_a), sinf(_a)); }
            { float _a = sharc_angle_to_rad((int32_t)args[8]); sharc_postmul_rz(cosf(_a), sinf(_a)); }
            g_sharc.ang[0] = g_sharc.ang[1] = g_sharc.ang[2] = 0;
            g_sharc.matrix_dirty = true;
            g_sharc.bone_dirty   = true;
            return;
        }

        /* 0x21804343: save_bone_to_PM_scratch — PM 0x02053C. Same semantics as 0x22004444.
         * 1 arg: N. Copy current 12-word bone (rot+pos, col-major) to pm_bone[N]. */
        case 0x21804343: {
            if (n >= 1) {
                int _s = (int)(args[0]) & 0xF;
                float (*r)[3] = g_sharc.rot; float *pm = g_sharc.pm_bone[_s];
                pm[0]=r[0][0]; pm[1]=r[0][1]; pm[2]=r[0][2];
                pm[3]=r[1][0]; pm[4]=r[1][1]; pm[5]=r[1][2];
                pm[6]=r[2][0]; pm[7]=r[2][1]; pm[8]=r[2][2];
                pm[9]=g_sharc.pos[0]; pm[10]=g_sharc.pos[1]; pm[11]=g_sharc.pos[2];
            }
            return;
        }

        /* 0x22804545: load_bone_from_PM_scratch — PM 0x02055C.
         * 1 arg: N. Copy pm_bone[N] (col-major 3x4) to current bone slot. */
        case 0x22804545: {
            if (n >= 1) {
                int _s = (int)(args[0]) & 0xF;
                float *pm = g_sharc.pm_bone[_s];
                g_sharc.rot[0][0]=pm[0]; g_sharc.rot[0][1]=pm[1]; g_sharc.rot[0][2]=pm[2];
                g_sharc.rot[1][0]=pm[3]; g_sharc.rot[1][1]=pm[4]; g_sharc.rot[1][2]=pm[5];
                g_sharc.rot[2][0]=pm[6]; g_sharc.rot[2][1]=pm[7]; g_sharc.rot[2][2]=pm[8];
                g_sharc.pos[0]=pm[9]; g_sharc.pos[1]=pm[10]; g_sharc.pos[2]=pm[11];
                g_sharc.matrix_dirty = true;
                g_sharc.bone_dirty   = true;
            }
            return;
        }

        /* 0x39807373: COP internal — PM 0x020D1F. 2 args, no output. */
        case 0x39807373:
            return;
        case 0x3A807575:
            sharc_push_u(0x00000000);
            sharc_push_u(0x0000FFFF);
            return;
        /* 0x3A007474: COP internal — PM 0x020D0A. 7 args, no output. */
        case 0x3A007474:
            return;

        /* 0x23004646: mul_bone_by_PM_scratch — PM 0x02056E.
         * 1 arg: N. Compose current bone with pm_bone[N]: new_bone = bone x pm_bone[N].
         * cpres1 _L2021F: DM[i7] x PM[i8] -> DM[i7] (col-major 3x4 multiplication). */
        case 0x23004646: {
            if (n >= 1) {
                int _s = (int)(args[0]) & 0xF;
                float (*r)[3] = g_sharc.rot; float *pm = g_sharc.pm_bone[_s];
                float nr[3][3]; float np[3]; int _c, _w;
                for (_c = 0; _c < 3; _c++)
                    for (_w = 0; _w < 3; _w++)
                        nr[_c][_w] = r[0][_w]*pm[_c*3] + r[1][_w]*pm[_c*3+1] + r[2][_w]*pm[_c*3+2];
                for (_w = 0; _w < 3; _w++)
                    np[_w] = g_sharc.pos[_w] + r[0][_w]*pm[9] + r[1][_w]*pm[10] + r[2][_w]*pm[11];
                memcpy(g_sharc.rot, nr, sizeof(nr));
                memcpy(g_sharc.pos, np, sizeof(np));
                g_sharc.matrix_dirty = true;
                g_sharc.bone_dirty   = true;
            }
            return;
        }

        /* 0x2B805757: read_col1 — PM 0x02044E.
         * Advances I7 by 3 (past col0) then reads slot[3..5] = rot[1][0..2] → 3 FIFO words. */
        case 0x2B805757:
            sharc_push_f(g_sharc.rot[1][0]);
            sharc_push_f(g_sharc.rot[1][1]);
            sharc_push_f(g_sharc.rot[1][2]);
            return;

        /* 0x2C005858: read_col2 — PM 0x020457.
         * Advances I7 by 6 (past col0+col1) then reads slot[6..8] = rot[2][0..2] → 3 FIFO words.
         * Must use rot[] directly — matrix[][2] has z-negation applied, which callers don't expect. */
        case 0x2C005858:
            sharc_push_f(g_sharc.rot[2][0]);
            sharc_push_f(g_sharc.rot[2][1]);
            sharc_push_f(g_sharc.rot[2][2]);
            return;

        /* 0x11802323: get_tan — PM 0x02061C.
         * 1 arg = i16 fixed-point angle.  Firmware: CALL sincos, CALL 0x0205D0 (RECIPS+NR divide),
         * result = sin/cos = tan(angle).  Returns 1 float. */
        case 0x11802323:
            if (n >= 1) {
                float a = sharc_angle_to_rad(args[0]);
                float c = cosf(a);
                float r = (c != 0.0f) ? (sinf(a) / c) : 0.0f;
                SANITIZE(r);
                sharc_push_f(r);
            }
            return;

        /* 0x25004A4A: read_anim_data — PM 0x02076E.
         * arg0 = SHARC DM word offset into DM[0x01400000] (= i960 BUFF_RAM at 0x00900000).
         * Firmware builds DM address = 0x01400000 + arg, then loops reading type-word blocks:
         *   type 0 (1 word):  end of stream — RTS
         *   type 1 (37 words): load 3×12-word col-major matrices; first 12 → current slot
         *   type 2 (31 words): 30 words → DM[0x30342] (scratch, no HLE state)
         *   type 3 ( 3 words):  2 words → DM[0x30360] (scratch)
         *   type 4 ( 4 words):  3 words → DM[0x30362] (scratch)
         *   type 5 (12 words): 11 words + CALL 0x020173 (bone normalize); stub: skip
         * Each type word is pushed to the i960 FIFO before dispatch (firmware DM(M0,I1)=R0). */
        case 0x25004A4A: {
            if (n < 1 || !g_sharc.sharc_dm_ext) { sharc_push_u(0); return; }
            uint32_t ptr = args[0];
            uint32_t wsz = g_sharc.sharc_dm_ext_size / 4;
            int _iter;
            for (_iter = 0; _iter < 64; _iter++) {
                uint32_t type;
                if (ptr >= wsz) break;
                memcpy(&type, g_sharc.sharc_dm_ext + ptr * 4, 4);
                sharc_push_u(type);
                ptr++;
                if (type == 0) break;     /* end of stream */
                if (type == 1) {
                    /* Load 12-word col-major matrix into current rot[]/pos[]. */
                    if (ptr + 36 > wsz) break;
                    {   float (*_r)[3] = g_sharc.rot;
                        int _c, _rw;
                        for (_c = 0; _c < 3; _c++)
                            for (_rw = 0; _rw < 3; _rw++) {
                                uint32_t _b;
                                memcpy(&_b, g_sharc.sharc_dm_ext + (ptr + _c*3+_rw)*4, 4);
                                _r[_c][_rw] = sharc_bits_to_float(_b);
                            }
                        for (_c = 0; _c < 3; _c++) {
                            uint32_t _b;
                            memcpy(&_b, g_sharc.sharc_dm_ext + (ptr+9+_c)*4, 4);
                            g_sharc.pos[_c] = sharc_bits_to_float(_b);
                        }
                    }
                    g_sharc.matrix_dirty = true;
                    g_sharc.bone_dirty   = true;
                    ptr += 36;   /* 3 × 12 data words */
                } else if (type == 2) { ptr += 30; }  /* → DM[0x30342] */
                else if (type == 3) { ptr += 2;  }    /* → DM[0x30360] */
                else if (type == 4) { ptr += 3;  }    /* → DM[0x30362] */
                else if (type == 5) { ptr += 11; }    /* 12-word block; CALL 0x020173 stub */
                else break;                            /* unknown type */
            }
            return;
        }

        /* 0x33806767: copy current 12-word bone slot (rot 3×3 + pos) to BUFF_RAM.
         * Firmware PM 0x020597: I7 = DM[0x3033F] (current slot ptr),
         * I6 = DM[0x01400000 + arg0], LCNTR=12, DM(I6,1)=DM(I7,1).
         * arg0 = word offset into BUFF_RAM (e.g. 0x3A00 for P1 bone 0).
         * Also snapshots to shadow_rot for the kage-matrix (arg=0x3D00) path. */
        case 0x33806767: {
            for (int _c = 0; _c < 3; _c++)
                for (int _r = 0; _r < 3; _r++)
                    g_sharc.shadow_rot[_c][_r] = g_sharc.rot[_c][_r];
            if (n >= 1 && g_sharc.sharc_dm_ext) {
                uint32_t byte_off = args[0] * 4;
                if (byte_off + 12 * 4 <= g_sharc.sharc_dm_ext_size) {
                    /* Write 9 col-major rotation words then 3 translation words. */
                    for (int _c = 0; _c < 3; _c++)
                        for (int _r = 0; _r < 3; _r++) {
                            uint32_t u = sharc_float_to_bits(g_sharc.rot[_c][_r]);
                            memcpy(g_sharc.sharc_dm_ext + byte_off + (_c*3+_r)*4, &u, 4);
                        }
                    for (int _i = 0; _i < 3; _i++) {
                        uint32_t u = sharc_float_to_bits(g_sharc.pos[_i]);
                        memcpy(g_sharc.sharc_dm_ext + byte_off + (9+_i)*4, &u, 4);
                    }

                }
            }
            return;
        }

        case 0x16002C2C: {
            /* 3D distance between two points.
             * PM 0x020670; IDA 0x671A8: st r4,g4,r5,g5,r6,g6 → ld r10
             * Args interleaved: (x1,x2,y1,y2,z1,z2). */
            float x1, x2, y1, y2, z1, z2;
            memcpy(&x1, &args[0], 4); memcpy(&x2, &args[1], 4);
            memcpy(&y1, &args[2], 4); memcpy(&y2, &args[3], 4);
            memcpy(&z1, &args[4], 4); memcpy(&z2, &args[5], 4);
            float dx = x1-x2, dy = y1-y2, dz = z1-z2;
            float r = sqrtf(dx*dx + dy*dy + dz*dz);
            SANITIZE(r);
            sharc_push_f(r);
            return;
        }
        case 0x18803131: {
            /* FV PM 0x020E80: F8=b-a, F1=F8*t, CALL fdiv(F1,span)→F0, result=F0+a.
             * arg3 = span: the full interpolation range; formula = a + (b-a)*t/span.
             * IDA 0x2B6E0: st r3,r4,r5,r6 → ld r3. */
            float a, b, t, span;
            memcpy(&a,    &args[0], 4);
            memcpy(&b,    &args[1], 4);
            memcpy(&t,    &args[2], 4);
            memcpy(&span, &args[3], 4);
            float r = (span != 0.0f) ? (a + (b - a) * t / span) : a;
            SANITIZE(r);
            sharc_push_f(r);
            return;
        }
        case 0x28805151: {
            /* Bone slot readback — 0 args, 3 16-bit results.
             * PM 0x020EF5; IDA 0x17CD4: ldis g0,g1,g2 from output FIFO.
             * Reads rot matrix entries from current bone slot via DM[0x3033F].
             * Stub: push 3 zeros until full bone-slot state is wired. */
            sharc_push_u(0);
            sharc_push_u(0);
            sharc_push_u(0);
            return;
        }
        case 0x0D001A1A: {
            /* sqrt(arg0) → 1 float.
             * PM 0x0205AF; calls Newton-Raphson rsqrt (PM 0x020281) then F0*rsqrt(F0)=sqrt(F0).
             * IDA 0x32CA0: 1 st (arg) + 1 ld (result); guarded by bbs 0x1F (skip if negative). */
            float a;
            memcpy(&a, &args[0], 4);
            float r = (a >= 0.0f) ? sqrtf(a) : 0.0f;
            SANITIZE(r);
            sharc_push_f(r);
            return;
        }

        default:
            g_sharc.unknown_cmds++;
            if ((cmd >> 16) == 0) return;
            {
                int found = 0;
                for (int _i = 0; _i < g_sharc.unknown_log_count; _i++) {
                    if (g_sharc.unknown_log[_i].cmd == cmd) {
                        g_sharc.unknown_log[_i].count++;
                        found = 1;
                        break;
                    }
                }
                if (!found && g_sharc.unknown_log_count < SHARC_UNKNOWN_LOG_MAX) {
                    g_sharc.unknown_log[g_sharc.unknown_log_count].cmd      = cmd;
                    g_sharc.unknown_log[g_sharc.unknown_log_count].first_ip = g_last_store_ip;
                    g_sharc.unknown_log[g_sharc.unknown_log_count].count    = 1;
                    g_sharc.unknown_log_count++;
                }
            }
            LOG_WARN("SHARC: unknown cmd 0x%08X (%d args) @ IP=0x%08X", cmd, n, g_last_store_ip);
            if (g_sharc.break_on_unknown) {
                g_sharc.unknown_triggered    = 1;
                g_sharc.unknown_trigger_cmd  = cmd;
                g_sharc.unknown_trigger_ip   = g_last_store_ip;
            }
            return;
    }
#undef SANITIZE
}

#endif /* SHARC_EXEC_H */
