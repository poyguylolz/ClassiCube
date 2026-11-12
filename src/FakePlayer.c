#include "FakePlayer.h"
#include "Commands.h"
#include "Entity.h"
#include "EntityComponents.h"
#include "Model.h"
#include "Chat.h"
#include "Game.h"
#include "Event.h"
#include "Stream.h"
#include "Bitmap.h"
#include "Graphics.h"
#include "Platform.h"
#include "String_.h"
#include "Utils.h"
#include "ExtMath.h"
#include "Constants.h"
#include "Funcs.h"
#include "World.h"

/*########################################################################################################################*
*-------------------------------------------------------State------------------------------------------------------------*
*#########################################################################################################################*/
#define FP_DEFAULT_NAME "FakePlayer"
#define FP_MODEL_NAME   "fakeplayer.bbmodel"
#define FP_MODELS_DIR   "models"
#define FP_MAX_FILE     (48 * 1024 * 1024)

static struct {
	cc_bool active;      /* whether a fake player currently exists */
	cc_bool removing;    /* true while we are the one removing it */
	cc_bool mimic;       /* copy the local player's movement and rotation */
	cc_bool rotateMove;  /* rotate copied movement by the offset too */
	cc_bool hasTex;      /* model uses an embedded/sibling texture instead of a skin */
	cc_bool taskAdded, eventsAdded;
	int id;              /* entity id (index into Entities.List / NetPlayers_List / TabList) */
	float offset;        /* yaw offset in degrees */
	Vec3 pos;            /* position the fake player is being driven to while mimicking */
	float texU, texV;    /* entity uScale/vScale needed because texture is padded to a power of 2 */
	struct CustomModel* cm;
	struct Bitmap bmp;   /* padded copy of the model texture, kept so it survives a graphics context loss */
	char modelDesc[FILENAME_SIZE]; /* what the model was loaded from, for /client fakeplayer status */
	char nameBuf[STRING_SIZE];
	cc_string name;
} fp;

static struct ModelTex fp_tex = { "fakeplayer.png", SKIN_64x64, 0, NULL };
static struct ScheduledTask2 fp_task;

static struct Entity* FP_Entity(void) {
	if (!fp.active) return NULL;
	return Entities.List[fp.id];
}

static void FP_Msg(const char* msg)  { Chat_AddRaw(msg); }
static cc_bool FP_RequireActive(void) {
	if (fp.active) return true;
	FP_Msg("&e/client: &cNo fake player yet. Use &f/client fakeplayer spawn &cfirst.");
	return false;
}


/*########################################################################################################################*
*---------------------------------------------------------JSON-----------------------------------------------------------*
*#########################################################################################################################*/
/* Minimal JSON reader, just enough for .bbmodel files. Strings are NOT unescaped, they are */
/* kept as (offset, length) spans into the original text, so even multi-megabyte embedded    */
/* base64 textures cost nothing to parse.                                                    */
enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

struct JNode {
	cc_uint8 type;
	int keyOff, keyLen; /* member name, when this node is inside an object */
	int off, len;       /* raw string span */
	double num;         /* number value, or 0/1 for bool */
	int first, last, next;
};
struct JDoc {
	const char* src;
	int len, pos;
	struct JNode* nodes;
	int count, cap;
	cc_bool failed;
};

static int J_New(struct JDoc* d, int type) {
	struct JNode* n;
	if (d->count == d->cap) {
		d->cap   = d->cap ? d->cap * 2 : 512;
		d->nodes = (struct JNode*)Mem_Realloc(d->nodes, d->cap, sizeof(struct JNode), "bbmodel json");
	}
	n = &d->nodes[d->count];
	Mem_Set(n, 0, sizeof(struct JNode));
	n->type  = (cc_uint8)type;
	n->first = -1; n->last = -1; n->next = -1;
	return d->count++;
}

static void J_SkipWS(struct JDoc* d) {
	while (d->pos < d->len) {
		char c = d->src[d->pos];
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { d->pos++; } else break;
	}
}

static cc_bool J_ReadSpan(struct JDoc* d, int* off, int* len) {
	if (d->pos >= d->len || d->src[d->pos] != '"') return false;
	d->pos++;
	*off = d->pos;

	while (d->pos < d->len) {
		char c = d->src[d->pos];
		if (c == '\\') { d->pos += 2; continue; }
		if (c == '"')  { *len = d->pos - *off; d->pos++; return true; }
		d->pos++;
	}
	return false;
}

static double J_ToDouble(const char* s, int n) {
	double v = 0.0, frac = 0.1, sign = 1.0;
	int i = 0, e = 0, esign = 1;

	if (i < n && s[i] == '-') { sign = -1.0; i++; }
	else if (i < n && s[i] == '+') i++;

	while (i < n && s[i] >= '0' && s[i] <= '9') { v = v * 10.0 + (s[i] - '0'); i++; }
	if (i < n && s[i] == '.') {
		i++;
		while (i < n && s[i] >= '0' && s[i] <= '9') { v += (s[i] - '0') * frac; frac *= 0.1; i++; }
	}
	if (i < n && (s[i] == 'e' || s[i] == 'E')) {
		i++;
		if (i < n && s[i] == '-') { esign = -1; i++; }
		else if (i < n && s[i] == '+') i++;
		while (i < n && s[i] >= '0' && s[i] <= '9') { e = e * 10 + (s[i] - '0'); i++; if (e > 300) break; }
		while (e-- > 0) v = esign > 0 ? v * 10.0 : v / 10.0;
	}
	return sign * v;
}

static int J_Parse(struct JDoc* d, int depth);
static void J_Link(struct JDoc* d, int parent, int child) {
	if (d->nodes[parent].first < 0) d->nodes[parent].first = child;
	else d->nodes[d->nodes[parent].last].next = child;
	d->nodes[parent].last = child;
}

static int J_Parse(struct JDoc* d, int depth) {
	int idx, child, off, len, start;
	char c;

	J_SkipWS(d);
	if (d->failed || depth > 64 || d->pos >= d->len) { d->failed = true; return -1; }
	c = d->src[d->pos];

	if (c == '{') {
		d->pos++;
		idx = J_New(d, J_OBJ);
		for (;;) {
			J_SkipWS(d);
			if (d->pos < d->len && d->src[d->pos] == '}') { d->pos++; break; }
			if (!J_ReadSpan(d, &off, &len)) { d->failed = true; return -1; }
			J_SkipWS(d);
			if (d->pos >= d->len || d->src[d->pos] != ':') { d->failed = true; return -1; }
			d->pos++;

			child = J_Parse(d, depth + 1);
			if (child < 0) { d->failed = true; return -1; }
			d->nodes[child].keyOff = off;
			d->nodes[child].keyLen = len;
			J_Link(d, idx, child);

			J_SkipWS(d);
			if (d->pos < d->len && d->src[d->pos] == ',') { d->pos++; continue; }
			if (d->pos < d->len && d->src[d->pos] == '}') { d->pos++; break; }
			d->failed = true; return -1;
		}
		return idx;
	}
	if (c == '[') {
		d->pos++;
		idx = J_New(d, J_ARR);
		for (;;) {
			J_SkipWS(d);
			if (d->pos < d->len && d->src[d->pos] == ']') { d->pos++; break; }
			child = J_Parse(d, depth + 1);
			if (child < 0) { d->failed = true; return -1; }
			J_Link(d, idx, child);

			J_SkipWS(d);
			if (d->pos < d->len && d->src[d->pos] == ',') { d->pos++; continue; }
			if (d->pos < d->len && d->src[d->pos] == ']') { d->pos++; break; }
			d->failed = true; return -1;
		}
		return idx;
	}
	if (c == '"') {
		if (!J_ReadSpan(d, &off, &len)) { d->failed = true; return -1; }
		idx = J_New(d, J_STR);
		d->nodes[idx].off = off;
		d->nodes[idx].len = len;
		return idx;
	}
	if (c == 't' && d->pos + 4 <= d->len) { d->pos += 4; idx = J_New(d, J_BOOL); d->nodes[idx].num = 1; return idx; }
	if (c == 'f' && d->pos + 5 <= d->len) { d->pos += 5; idx = J_New(d, J_BOOL); d->nodes[idx].num = 0; return idx; }
	if (c == 'n' && d->pos + 4 <= d->len) { d->pos += 4; return J_New(d, J_NULL); }

	start = d->pos;
	while (d->pos < d->len) {
		c = d->src[d->pos];
		if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') d->pos++;
		else break;
	}
	if (d->pos == start) { d->failed = true; return -1; }
	idx = J_New(d, J_NUM);
	d->nodes[idx].num = J_ToDouble(d->src + start, d->pos - start);
	return idx;
}

/* Returns member 'key' of object 'obj', or -1 */
static int J_Key(struct JDoc* d, int obj, const char* key) {
	int child, i, n = 0;
	if (obj < 0 || d->nodes[obj].type != J_OBJ) return -1;
	while (key[n]) n++;

	for (child = d->nodes[obj].first; child >= 0; child = d->nodes[child].next)
	{
		struct JNode* c = &d->nodes[child];
		if (c->keyLen != n) continue;
		for (i = 0; i < n; i++) { if (d->src[c->keyOff + i] != key[i]) break; }
		if (i == n) return child;
	}
	return -1;
}

static int J_Nth(struct JDoc* d, int arr, int n) {
	int child;
	if (arr < 0 || d->nodes[arr].type != J_ARR) return -1;
	for (child = d->nodes[arr].first; child >= 0; child = d->nodes[child].next)
	{
		if (n-- == 0) return child;
	}
	return -1;
}

static int J_Count(struct JDoc* d, int arr) {
	int child, n = 0;
	if (arr < 0 || d->nodes[arr].type != J_ARR) return 0;
	for (child = d->nodes[arr].first; child >= 0; child = d->nodes[child].next) n++;
	return n;
}

static float J_Float(struct JDoc* d, int node, float def) {
	if (node < 0 || d->nodes[node].type != J_NUM) return def;
	return (float)d->nodes[node].num;
}

static cc_bool J_Bool(struct JDoc* d, int node, cc_bool def) {
	if (node < 0 || d->nodes[node].type != J_BOOL) return def;
	return d->nodes[node].num != 0;
}

static void J_Vec3(struct JDoc* d, int arr, Vec3* v, float def) {
	v->x = J_Float(d, J_Nth(d, arr, 0), def);
	v->y = J_Float(d, J_Nth(d, arr, 1), def);
	v->z = J_Float(d, J_Nth(d, arr, 2), def);
}

static cc_bool J_SpanEq(struct JDoc* d, int a, int b) {
	int i;
	if (a < 0 || b < 0) return false;
	if (d->nodes[a].type != J_STR || d->nodes[b].type != J_STR) return false;
	if (d->nodes[a].len != d->nodes[b].len) return false;
	for (i = 0; i < d->nodes[a].len; i++) {
		if (d->src[d->nodes[a].off + i] != d->src[d->nodes[b].off + i]) return false;
	}
	return true;
}


/*########################################################################################################################*
*-----------------------------------------------------Model building-----------------------------------------------------*
*#########################################################################################################################*/
enum { FPANIM_NONE, FPANIM_HEAD, FPANIM_LARM, FPANIM_RARM, FPANIM_LLEG, FPANIM_RLEG };

struct FPCtx {
	cc_bool hasRot;
	Vec3 rot, rotOrigin;   /* nearest ancestor group that has a rotation */
	int anim;
	Vec3 animOrigin;       /* pivot of the nearest ancestor group that is a head/arm/leg */
};
struct FPBounds { Vec3 min, max; cc_bool any; };

static cc_bool FP_Has(const char* hay, const char* needle) {
	int i, j;
	for (i = 0; hay[i]; i++)
	{
		for (j = 0; needle[j] && hay[i + j] == needle[j]; j++) { }
		if (!needle[j]) return true;
	}
	return false;
}
static cc_bool FP_Starts(const char* hay, const char* needle) {
	int j;
	for (j = 0; needle[j]; j++) { if (hay[j] != needle[j]) return false; }
	return true;
}

/* Works out whether a group/cube name means "this is the head / an arm / a leg" so it gets */
/* the same walking animation as a normal player. Names like "Left Arm", "left_arm", "arm_L" work. */
static int FP_ClassifyName(struct JDoc* d, int nameNode) {
	char buf[64];
	int i, n = 0;
	char c;

	if (nameNode < 0 || d->nodes[nameNode].type != J_STR) return FPANIM_NONE;
	for (i = 0; i < d->nodes[nameNode].len && n < 63; i++)
	{
		c = d->src[d->nodes[nameNode].off + i];
		if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
		if (c >= 'a' && c <= 'z') buf[n++] = c;
	}
	buf[n] = '\0';

	if (FP_Has(buf, "leftarm")  || FP_Has(buf, "armleft")  || FP_Starts(buf, "larm")) return FPANIM_LARM;
	if (FP_Has(buf, "rightarm") || FP_Has(buf, "armright") || FP_Starts(buf, "rarm")) return FPANIM_RARM;
	if (FP_Has(buf, "leftleg")  || FP_Has(buf, "legleft")  || FP_Starts(buf, "lleg")) return FPANIM_LLEG;
	if (FP_Has(buf, "rightleg") || FP_Has(buf, "legright") || FP_Starts(buf, "rleg")) return FPANIM_RLEG;
	if (FP_Has(buf, "head")) return FPANIM_HEAD;
	return FPANIM_NONE;
}

static void FP_SetAnims(struct CustomModelPart* part, int anim) {
	switch (anim) {
	case FPANIM_HEAD:
		part->animType[0] = CustomModelAnimType_Head;      part->animAxis[0] = CustomModelAnimAxis_X;
		break;
	case FPANIM_LARM:
		part->animType[0] = CustomModelAnimType_LeftArmX;  part->animAxis[0] = CustomModelAnimAxis_X;
		part->animType[1] = CustomModelAnimType_LeftArmZ;  part->animAxis[1] = CustomModelAnimAxis_Z;
		break;
	case FPANIM_RARM:
		part->animType[0] = CustomModelAnimType_RightArmX; part->animAxis[0] = CustomModelAnimAxis_X;
		part->animType[1] = CustomModelAnimType_RightArmZ; part->animAxis[1] = CustomModelAnimAxis_Z;
		break;
	case FPANIM_LLEG:
		part->animType[0] = CustomModelAnimType_LeftLegX;  part->animAxis[0] = CustomModelAnimAxis_X;
		break;
	case FPANIM_RLEG:
		part->animType[0] = CustomModelAnimType_RightLegX; part->animAxis[0] = CustomModelAnimAxis_X;
		break;
	}
}

static cc_uint16 FP_UV(float v) {
	int i = (int)(v + 0.5f);
	if (i < 0) i = 0;
	if (i > 0x7FFF) i = 0x7FFF;
	return (cc_uint16)i;
}

static cc_bool FP_IsZero3(const Vec3* v) { return v->x == 0.0f && v->y == 0.0f && v->z == 0.0f; }

/* Blockbench face names, in ClassiCube's face order: top, bottom, front, back, left, right */
static const char* const fp_faceNames[6] = { "up", "down", "north", "south", "east", "west" };

static int fp_skipped; /* number of elements that couldn't be turned into parts */

static void FP_AddCube(struct JDoc* d, int el, const struct FPCtx* ctx, struct CustomModel* cm, struct FPBounds* b) {
	struct CustomModelPartDef def;
	struct CustomModelPart* part;
	Vec3 from, to, mn, mx, origin, rot, rotOrigin, pivot;
	float inflate, t, uv[4];
	int faces, f, k, node, anim, type;

	type = J_Key(d, el, "type");
	if (type >= 0 && d->nodes[type].type == J_STR) {
		/* meshes etc. can't be represented by a ClassiCube model part */
		if (!(d->nodes[type].len == 4 && d->src[d->nodes[type].off] == 'c')) { fp_skipped++; return; }
	}
	if (!J_Bool(d, J_Key(d, el, "visibility"), true)) return;
	if (!J_Bool(d, J_Key(d, el, "export"),     true)) return;
	if (cm->curPartIndex >= MAX_CUSTOM_MODEL_PARTS) { fp_skipped++; return; }

	J_Vec3(d, J_Key(d, el, "from"),     &from,   0.0f);
	J_Vec3(d, J_Key(d, el, "to"),       &to,     0.0f);
	J_Vec3(d, J_Key(d, el, "origin"),   &origin, 0.0f);
	J_Vec3(d, J_Key(d, el, "rotation"), &rot,    0.0f);
	inflate = J_Float(d, J_Key(d, el, "inflate"), 0.0f);

	mn = from; mx = to;
	if (mn.x > mx.x) { t = mn.x; mn.x = mx.x; mx.x = t; }
	if (mn.y > mx.y) { t = mn.y; mn.y = mx.y; mx.y = t; }
	if (mn.z > mx.z) { t = mn.z; mn.z = mx.z; mx.z = t; }
	mn.x -= inflate; mn.y -= inflate; mn.z -= inflate;
	mx.x += inflate; mx.y += inflate; mx.z += inflate;

	/* Which rotation/pivot to use. A ClassiCube part has exactly one rotation, so a cube inside a */
	/* rotated group uses the group's rotation, and a cube with its own rotation uses that.        */
	rotOrigin = origin;
	if (FP_IsZero3(&rot) && ctx->hasRot) { rot = ctx->rot; rotOrigin = ctx->rotOrigin; }

	anim  = ctx->anim;
	pivot = ctx->animOrigin;
	if (anim == FPANIM_NONE) {
		anim  = FP_ClassifyName(d, J_Key(d, el, "name"));
		pivot = origin;
	}
	/* limbs must rotate around their joint so the walk animation looks right */
	if (anim != FPANIM_NONE) rotOrigin = pivot;

	Mem_Set(&def, 0, sizeof(def));
	def.min.x = mn.x / 16.0f; def.min.y = mn.y / 16.0f; def.min.z = mn.z / 16.0f;
	def.max.x = mx.x / 16.0f; def.max.y = mx.y / 16.0f; def.max.z = mx.z / 16.0f;
	def.rotationOrigin.x = rotOrigin.x / 16.0f;
	def.rotationOrigin.y = rotOrigin.y / 16.0f;
	def.rotationOrigin.z = rotOrigin.z / 16.0f;

	faces = J_Key(d, el, "faces");
	for (f = 0; f < 6; f++)
	{
		node = faces >= 0 ? J_Key(d, faces, fp_faceNames[f]) : -1;
		node = J_Key(d, node, "uv");
		for (k = 0; k < 4; k++) uv[k] = J_Float(d, J_Nth(d, node, k), 0.0f);

		/* Blockbench uv = [u1,v1,u2,v2] with (u1,v1) being the top left of the face as seen from outside. */
		/* ClassiCube builds its quads the other way round horizontally for the 4 sides and the bottom,    */
		/* and vertically for the top (this matches how Model.c's own box builder lays out a skin).        */
		if (f == 0) { /* top */
			def.u1[f] = FP_UV(uv[0]); def.v1[f] = FP_UV(uv[3]);
			def.u2[f] = FP_UV(uv[2]); def.v2[f] = FP_UV(uv[1]);
		} else {      /* bottom, front, back, left, right */
			def.u1[f] = FP_UV(uv[2]); def.v1[f] = FP_UV(uv[1]);
			def.u2[f] = FP_UV(uv[0]); def.v2[f] = FP_UV(uv[3]);
		}
	}

	part = &cm->parts[cm->curPartIndex];
	part->rotation = rot;
	FP_SetAnims(part, anim);
	CustomModel_BuildPart(cm, &def);
	cm->curPartIndex++;

	if (!b->any || def.min.x < b->min.x) b->min.x = def.min.x;
	if (!b->any || def.min.y < b->min.y) b->min.y = def.min.y;
	if (!b->any || def.min.z < b->min.z) b->min.z = def.min.z;
	if (!b->any || def.max.x > b->max.x) b->max.x = def.max.x;
	if (!b->any || def.max.y > b->max.y) b->max.y = def.max.y;
	if (!b->any || def.max.z > b->max.z) b->max.z = def.max.z;
	b->any = true;
}

struct FPWalk {
	struct JDoc* d;
	int* elems;           /* node index of each element */
	cc_uint8* visited;
	int numElems;
	struct CustomModel* cm;
	struct FPBounds* bounds;
};

static void FP_WalkOutliner(struct FPWalk* w, int arr, const struct FPCtx* ctx, int depth) {
	struct JDoc* d = w->d;
	struct FPCtx sub;
	int child, i, uuid, nameNode, kids, cls;
	Vec3 rot, origin;

	if (arr < 0 || d->nodes[arr].type != J_ARR) return;
	for (child = d->nodes[arr].first; child >= 0; child = d->nodes[child].next)
	{
		if (d->nodes[child].type == J_STR) {
			for (i = 0; i < w->numElems; i++)
			{
				uuid = J_Key(d, w->elems[i], "uuid");
				if (w->visited[i] || !J_SpanEq(d, uuid, child)) continue;
				w->visited[i] = 1;
				FP_AddCube(d, w->elems[i], ctx, w->cm, w->bounds);
				break;
			}
		} else if (d->nodes[child].type == J_OBJ && depth < 32) {
			if (!J_Bool(d, J_Key(d, child, "visibility"), true)) continue;
			if (!J_Bool(d, J_Key(d, child, "export"),     true)) continue;

			sub = *ctx;
			J_Vec3(d, J_Key(d, child, "origin"),   &origin, 0.0f);
			J_Vec3(d, J_Key(d, child, "rotation"), &rot,    0.0f);
			if (!FP_IsZero3(&rot)) { sub.hasRot = true; sub.rot = rot; sub.rotOrigin = origin; }

			nameNode = J_Key(d, child, "name");
			cls      = FP_ClassifyName(d, nameNode);
			if (cls != FPANIM_NONE && sub.anim == FPANIM_NONE) { sub.anim = cls; sub.animOrigin = origin; }

			kids = J_Key(d, child, "children");
			FP_WalkOutliner(w, kids, &sub, depth + 1);
		}
	}
}


/*########################################################################################################################*
*---------------------------------------------------------Files----------------------------------------------------------*
*#########################################################################################################################*/
static cc_bool FP_TryRead(const cc_string* path, char** data, int* len) {
	struct Stream s;
	cc_uint32 size = 0;
	cc_result res;
	char* buf;

	if (Stream_OpenFile(&s, path)) return false;
	if (s.Length(&s, &size) || size == 0 || size > FP_MAX_FILE) { s.Close(&s); return false; }

	buf = (char*)Mem_TryAlloc(size + 1, 1);
	if (!buf) { s.Close(&s); return false; }

	res = Stream_Read(&s, (cc_uint8*)buf, size);
	s.Close(&s);
	if (res) { Mem_Free(buf); return false; }

	buf[size] = '\0';
	*data = buf;
	*len  = (int)size;
	return true;
}

static cc_bool FP_HasExt(const cc_string* s, const char* ext, int extLen) {
	cc_string tail;
	if (s->length < extLen) return false;
	tail = String_UNSAFE_SubstringAt(s, s->length - extLen);
	return String_CaselessEqualsConst(&tail, ext) != 0;
}

/* Looks for the model file. If the user gave a directory, that is tried first. If it isn't */
/* there (or no directory was given) the "models" folder is checked. Otherwise it fails.     */
static cc_bool FP_FindModelFile(const cc_string* arg_, char** data, int* len, cc_string* usedPath) {
	cc_string arg = *arg_;
	cc_string base, tmp;
	char tmpBuf[FILENAME_SIZE];
	cc_bool hasDir;
	int i, pass, slash;

	/* strip quotes the user may have pasted with the path */
	while (arg.length && (arg.buffer[0] == '"' || arg.buffer[0] == ' ')) arg = String_UNSAFE_SubstringAt(&arg, 1);
	while (arg.length && (arg.buffer[arg.length - 1] == '"' || arg.buffer[arg.length - 1] == ' ')) arg.length--;
	if (!arg.length) return false;

	hasDir = String_IndexOf(&arg, '/') >= 0 || String_IndexOf(&arg, '\\') >= 0 || String_IndexOf(&arg, ':') >= 0;
	slash  = max(String_LastIndexOf(&arg, '/'), String_LastIndexOf(&arg, '\\'));
	base   = String_UNSAFE_SubstringAt(&arg, slash + 1);

	/* pass 0 = the path as given (only if it has a directory), pass 1 = models folder */
	for (pass = hasDir ? 0 : 1; pass < 2; pass++)
	{
		for (i = 0; i < 2; i++)
		{
			String_InitArray(tmp, tmpBuf);
			if (pass == 0) {
				String_AppendString(&tmp, &arg);
			} else {
				String_AppendConst(&tmp, FP_MODELS_DIR "/");
				String_AppendString(&tmp, hasDir ? &base : &arg);
			}
			if (i == 1) {
				if (FP_HasExt(&tmp, ".bbmodel", 8)) continue;
				String_AppendConst(&tmp, ".bbmodel");
			}

			if (!FP_TryRead(&tmp, data, len)) continue;
			String_Copy(usedPath, &tmp);
			return true;
		}
	}
	return false;
}


/*########################################################################################################################*
*---------------------------------------------------------Textures-------------------------------------------------------*
*#########################################################################################################################*/
static void FP_FreeTexture(void) {
	Gfx_DeleteTexture(&fp_tex.texID);
	if (fp.bmp.scan0) Mem_Free(fp.bmp.scan0);
	fp.bmp.scan0 = NULL;
	fp.bmp.width = 0; fp.bmp.height = 0;
}

static void FP_UploadTexture(void) {
	if (!fp.bmp.scan0) return;
	if (fp_tex.texID)  Gfx_DeleteTexture(&fp_tex.texID);
	fp_tex.texID = Gfx_CreateTexture(&fp.bmp, TEXTURE_FLAG_MANAGED, false);
}

/* Decodes a PNG from memory and pads it to power of 2 dimensions. */
static cc_bool FP_DecodePNG(const cc_uint8* data, int len, struct Bitmap* out, int* origW, int* origH) {
	struct Stream s;
	struct Bitmap bmp, padded;
	int w, h, y;

	Stream_ReadonlyMemory(&s, (void*)data, (cc_uint32)len);
	if (Png_Decode(&bmp, &s)) return false;

	w = Math_NextPowOf2(bmp.width);
	h = Math_NextPowOf2(bmp.height);
	*origW = bmp.width; *origH = bmp.height;

	if (!Gfx_CheckTextureSize(w, h, 0)) { Mem_Free(bmp.scan0); return false; }
	if (w == bmp.width && h == bmp.height) { *out = bmp; return true; }

	padded.width = w; padded.height = h;
	padded.scan0 = (BitmapCol*)Mem_TryAllocCleared(w * h, BITMAPCOLOR_SIZE);
	if (!padded.scan0) { Mem_Free(bmp.scan0); return false; }

	for (y = 0; y < bmp.height; y++)
	{
		Mem_Copy(Bitmap_GetRow(&padded, y), Bitmap_GetRow(&bmp, y), bmp.width * BITMAPCOLOR_SIZE);
	}
	Mem_Free(bmp.scan0);
	*out = padded;
	return true;
}

/* Finds the model's texture: embedded base64 PNG first, then a .png with the same name next to the file */
static cc_bool FP_LoadTexture(struct JDoc* d, const cc_string* modelPath, struct Bitmap* out, int* w, int* h) {
	int tex, src, comma, i, dataOff, dataLen, size;
	cc_uint8* raw;
	cc_bool ok = false;

	tex = J_Nth(d, J_Key(d, 0, "textures"), 0);
	src = J_Key(d, tex, "source");

	if (src >= 0 && d->nodes[src].type == J_STR) {
		comma = -1;
		for (i = 0; i < d->nodes[src].len && i < 128; i++)
		{
			if (d->src[d->nodes[src].off + i] == ',') { comma = i; break; }
		}
		if (comma >= 0) {
			dataOff = d->nodes[src].off + comma + 1;
			dataLen = d->nodes[src].len - comma - 1;
			raw = (cc_uint8*)Mem_TryAlloc(dataLen + 8, 1);
			if (raw) {
				size = Convert_FromBase64(d->src + dataOff, dataLen, raw);
				if (size > 0) ok = FP_DecodePNG(raw, size, out, w, h);
				Mem_Free(raw);
			}
			if (ok) return true;
		}
	}

	{
		/* sibling file: models/steve.bbmodel -> models/steve.png */
		cc_string path; char pathBuf[FILENAME_SIZE];
		char* data; int len;
		String_InitArray(path, pathBuf);
		String_AppendString(&path, modelPath);
		if (FP_HasExt(&path, ".bbmodel", 8)) path.length -= 8;
		String_AppendConst(&path, ".png");

		if (FP_TryRead(&path, &data, &len)) {
			ok = FP_DecodePNG((cc_uint8*)data, len, out, w, h);
			Mem_Free(data);
		}
	}
	return ok;
}


/*########################################################################################################################*
*------------------------------------------------------Model loading-----------------------------------------------------*
*#########################################################################################################################*/
static void FP_FreeModel(void) {
	if (fp.cm) {
		CustomModel_Undefine(fp.cm); /* also resets any entity using it back to humanoid */
		Mem_Free(fp.cm);
		fp.cm = NULL;
	}
	FP_FreeTexture();
	fp.hasTex = false;
	fp.modelDesc[0] = '\0';
}

static void FP_ApplyName(const cc_string* name);

static void FP_UseHumanoid(void) {
	static const cc_string human = String_FromConst("humanoid");
	struct Entity* e = FP_Entity();

	FP_FreeModel();
	if (!e) return;

	Entity_SetModel(e, &human);
	e->uScale = 1.0f; e->vScale = 1.0f;
	Entity_SetSkin(e, &fp.name); /* skin is the player name, like a normal player */
}

/* Returns NULL on success, otherwise an error message */
static const char* FP_LoadBBModel(const cc_string* arg) {
	cc_string modelName = String_FromConst(FP_MODEL_NAME);
	cc_string usedPath; char usedBuf[FILENAME_SIZE];
	struct JDoc doc;
	struct FPCtx ctx;
	struct FPWalk walk;
	struct FPBounds bounds;
	struct CustomModel* cm;
	struct Bitmap bmp;
	char* text = NULL;
	int textLen = 0, texW = 0, texH = 0, i, el, elements, outliner, res;
	cc_bool hasTex;
	const char* err = NULL;
	struct Entity* e = FP_Entity();

	String_InitArray(usedPath, usedBuf);
	if (!FP_FindModelFile(arg, &text, &textLen, &usedPath)) return "not found";

	Mem_Set(&doc, 0, sizeof(doc));
	doc.src = text; doc.len = textLen;
	if (J_Parse(&doc, 0) < 0 || doc.failed || doc.nodes[0].type != J_OBJ) {
		err = "that file isn't valid JSON"; goto done_nomodel;
	}

	elements = J_Key(&doc, 0, "elements");
	if (J_Count(&doc, elements) == 0) { err = "no elements (cubes) in that model"; goto done_nomodel; }

	/* texture is decoded first, so a bad texture doesn't wreck the currently loaded model */
	Mem_Set(&bmp, 0, sizeof(bmp));
	hasTex = FP_LoadTexture(&doc, &usedPath, &bmp, &texW, &texH);

	cm = (struct CustomModel*)Mem_TryAllocCleared(1, sizeof(struct CustomModel));
	if (!cm) { err = "out of memory"; goto done_tex; }
	cm->model.vertices = (struct ModelVertex*)Mem_TryAllocCleared(MAX_CUSTOM_MODEL_PARTS * MODEL_BOX_VERTICES, sizeof(struct ModelVertex));
	if (!cm->model.vertices) { Mem_Free(cm); err = "out of memory"; goto done_tex; }

	Model_Init(&cm->model);
	String_CopyToRawArray(cm->name, &modelName);
	cm->defined = true;

	/* collect elements */
	walk.d = &doc; walk.cm = cm; walk.bounds = &bounds;
	walk.numElems = J_Count(&doc, elements);
	walk.elems    = (int*)Mem_TryAllocCleared(walk.numElems, sizeof(int));
	walk.visited  = (cc_uint8*)Mem_TryAllocCleared(walk.numElems, 1);
	if (!walk.elems || !walk.visited) {
		if (walk.elems)   Mem_Free(walk.elems);
		if (walk.visited) Mem_Free(walk.visited);
		Mem_Free(cm->model.vertices); Mem_Free(cm);
		err = "out of memory"; goto done_tex;
	}
	for (i = 0, el = doc.nodes[elements].first; el >= 0; el = doc.nodes[el].next) walk.elems[i++] = el;

	Mem_Set(&bounds, 0, sizeof(bounds));
	Mem_Set(&ctx, 0, sizeof(ctx));
	fp_skipped = 0;

	outliner = J_Key(&doc, 0, "outliner");
	FP_WalkOutliner(&walk, outliner, &ctx, 0);
	/* anything the outliner didn't mention (or models with no outliner at all) */
	for (i = 0; i < walk.numElems; i++)
	{
		if (!walk.visited[i]) FP_AddCube(&doc, walk.elems[i], &ctx, cm, &bounds);
	}
	Mem_Free(walk.elems); Mem_Free(walk.visited);

	if (!cm->curPartIndex) {
		Mem_Free(cm->model.vertices); Mem_Free(cm);
		err = "no usable cubes in that model"; goto done_tex;
	}

	cm->numParts = cm->curPartIndex;
	res = (int)(J_Float(&doc, J_Key(&doc, J_Key(&doc, 0, "resolution"), "width"),  0.0f) + 0.5f);
	cm->uScale = (cc_uint16)(res > 0 ? res : (hasTex ? texW : 64));
	res = (int)(J_Float(&doc, J_Key(&doc, J_Key(&doc, 0, "resolution"), "height"), 0.0f) + 0.5f);
	cm->vScale = (cc_uint16)(res > 0 ? res : (hasTex ? texH : 64));

	cm->nameY = bounds.max.y + 0.075f;
	cm->eyeY  = bounds.max.y * 0.9f;
	cm->collisionBounds.x = max(bounds.max.x - bounds.min.x, 0.25f);
	cm->collisionBounds.y = max(bounds.max.y, 0.25f);
	cm->collisionBounds.z = max(bounds.max.z - bounds.min.z, 0.25f);
	cm->pickingBoundsAABB.Min = bounds.min;
	cm->pickingBoundsAABB.Max = bounds.max;

	cm->model.bobbing        = true;
	cm->model.pushes         = true;
	cm->model.calcHumanAnims = true;                /* gives walking animations to head/arm/leg groups */
	cm->model.usesHumanSkin  = !hasTex;             /* no texture in the file = use the player's skin  */
	cm->model.usesSkin       = !hasTex;

	/* everything worked, now swap it in */
	FP_FreeModel();
	fp.cm = cm;
	CustomModel_Register(cm);

	if (hasTex) {
		fp.bmp = bmp;
		fp.texU = (float)texW / bmp.width;
		fp.texV = (float)texH / bmp.height;
		fp.hasTex = true;
		bmp.scan0 = NULL; /* now owned by fp.bmp */
		FP_UploadTexture();
		cm->model.defaultTex = &fp_tex;
	}

	String_CopyToRawArray(fp.modelDesc, &usedPath);
	Entity_SetModel(e, &modelName);
	if (hasTex) {
		e->uScale = fp.texU; e->vScale = fp.texV;
		e->NonHumanSkin = false;
	} else {
		e->uScale = 1.0f; e->vScale = 1.0f;
		Entity_SetSkin(e, &fp.name);
	}

	if (fp_skipped) Chat_Add1("&e/client: &7Skipped %i element(s) that aren't plain cubes (or over the 64 part limit).", &fp_skipped);
	if (!hasTex)    FP_Msg("&e/client: &7No texture found in the model file, using the player skin instead.");
	err = NULL;
	goto done_tex;

done_tex:
	if (bmp.scan0) Mem_Free(bmp.scan0);
done_nomodel:
	if (doc.nodes) Mem_Free(doc.nodes);
	Mem_Free(text);
	return err;
}


/*########################################################################################################################*
*----------------------------------------------------Spawning / removing-------------------------------------------------*
*#########################################################################################################################*/
/* highest free id first, since servers hand out low ids first */
static int FP_FindFreeID(void) {
	int i;
	for (i = ENTITIES_SELF_ID - 1; i >= 0; i--)
	{
		if (!Entities.List[i] && !TabList.NameOffsets[i]) return i;
	}
	return -1;
}

static void FP_ApplyName(const cc_string* name) {
	static const cc_string group = String_FromConst("Players");
	struct Entity* e = FP_Entity();
	if (!e) return;

	String_Copy(&fp.name, name);
	Entity_SetName(e, &fp.name);
	/* name is also the skin, unless the model brings its own texture */
	if (!fp.hasTex) Entity_SetSkin(e, &fp.name);

	TabList_Set((EntityID)fp.id, &fp.name, &fp.name, &group, 0);
	TabList_EntityLinked_Set(fp.id);
}

static void FP_Teleport(const struct Entity* to) {
	struct Entity* e = FP_Entity();
	struct LocationUpdate u;
	if (!e) return;

	u.flags = LU_HAS_POS | LU_HAS_YAW | LU_HAS_PITCH | LU_POS_ABSOLUTE_INSTANT;
	u.pos   = to->Position;
	u.yaw   = Math_ClampAngle(to->Yaw + (fp.mimic ? fp.offset : 0.0f));
	u.pitch = to->Pitch;
	u.rotX  = 0; u.rotZ = 0;
	e->VTABLE->SetLocation(e, &u);
	fp.pos = u.pos;
}

static void FP_Cleanup(void) {
	/* tab list entry goes away on its own, it's linked to the entity */
	fp.active = false;
	fp.mimic  = false;
	FP_FreeModel();
}

/* Raised whenever any entity is removed. Covers us removing it, the server reusing our id, and disconnecting */
static void FP_OnEntityRemoved(void* obj, int id) {
	if (!fp.active || id != fp.id) return;
	if (!fp.removing) FP_Msg("&e/client: &7Fake player was removed (the server took its entity id, or you left the world).");
	FP_Cleanup();
}

static void FP_Remove(void) {
	fp.removing = true;
	Entities_Remove(fp.id); /* raises Removed -> FP_Cleanup */
	fp.removing = false;
	fp.active   = false;
}

static void FP_Spawn(const cc_string* name) {
	struct LocalPlayer* lp = Entities.CurPlayer;
	struct NetPlayer* np;
	struct Entity* e;
	int id;

	if (!World.Loaded) { FP_Msg("&e/client: &cJoin a world first."); return; }
	if (fp.active) FP_Remove();

	id = FP_FindFreeID();
	if (id < 0) { FP_Msg("&e/client: &cNo free entity id for a fake player."); return; }

	np = &NetPlayers_List[id];
	NetPlayer_Init(np);
	e  = &np->Base;
	Entities.List[id] = e;
	Event_RaiseInt(&EntityEvents.Added, id);

	fp.active = true; fp.id = id;
	fp.mimic  = false; fp.removing = false;
	fp.hasTex = false;
	String_InitArray(fp.name, fp.nameBuf);

	FP_ApplyName(name);
	FP_Teleport(&lp->Base);
}


/*########################################################################################################################*
*---------------------------------------------------------Mimicking------------------------------------------------------*
*#########################################################################################################################*/
/* Runs every game tick, after entities have ticked. Drives the fake player's *next* location */
/* directly so it interpolates in lock-step with the local player, with no network-style lag. */
static cc_bool FP_Tick(struct ScheduledTask2* task) {
	struct LocalPlayer* lp = Entities.CurPlayer;
	struct Entity* e;
	float dx, dy, dz, rx, rz, c, s, ang;

	if (!fp.active || !fp.mimic || !World.Loaded) return true;
	e = Entities.List[fp.id];
	if (!e || e != &NetPlayers_List[fp.id].Base) return true;

	/* how far the local player moved during their last tick */
	dx = lp->Base.next.pos.x - lp->Base.prev.pos.x;
	dy = lp->Base.next.pos.y - lp->Base.prev.pos.y;
	dz = lp->Base.next.pos.z - lp->Base.prev.pos.z;

	if (fp.rotateMove) {
		ang = fp.offset * MATH_DEG2RAD;
		c = Math_CosF(ang); s = Math_SinF(ang);
		rx = dx * c - dz * s;
		rz = dz * c + dx * s;
	} else {
		rx = dx; rz = dz;
	}
	fp.pos.x += rx; fp.pos.y += dy; fp.pos.z += rz;

	e->next.pos   = fp.pos;
	e->next.yaw   = Math_ClampAngle(lp->Base.Yaw + fp.offset);
	e->next.rotY  = e->next.yaw;
	e->next.pitch = lp->Base.Pitch;
	e->next.rotX  = 0.0f;
	e->next.rotZ  = 0.0f;
	return true;
}

static void FP_ContextLost(void* obj) {
	Gfx_DeleteTexture(&fp_tex.texID);
}
static void FP_ContextRecreated(void* obj) {
	FP_UploadTexture();
}

void FakePlayer_Init(void) {
	Mem_Set(&fp, 0, sizeof(fp));
	fp.rotateMove = true;

	fp_task.interval = GAME_DEF_TICKS;
	fp_task.callback = FP_Tick;
	ScheduledTask2_Add(&fp_task);

	Event_Register_(&EntityEvents.Removed,   NULL, FP_OnEntityRemoved);
	Event_Register_(&GfxEvents.ContextLost,      NULL, FP_ContextLost);
	Event_Register_(&GfxEvents.ContextRecreated, NULL, FP_ContextRecreated);
}

void FakePlayer_Free(void) {
	Event_Unregister_(&EntityEvents.Removed,   NULL, FP_OnEntityRemoved);
	Event_Unregister_(&GfxEvents.ContextLost,      NULL, FP_ContextLost);
	Event_Unregister_(&GfxEvents.ContextRecreated, NULL, FP_ContextRecreated);
	FP_Cleanup();
}


/*########################################################################################################################*
*---------------------------------------------------------Command--------------------------------------------------------*
*#########################################################################################################################*/
static void FP_PrintStatus(void) {
	const char* model;
	if (!fp.active) {
		FP_Msg("&e/client: &fNo fake player. &a/client fakeplayer spawn [name]");
		return;
	}
	model = fp.modelDesc[0] ? fp.modelDesc : "humanoid";
	Chat_Add2("&e/client: &fFake player &a%s &f(id %i)", &fp.name, &fp.id);
	Chat_Add1("&e  model: &f%c", model);
	Chat_Add1("&e  mimic: &f%c", fp.mimic ? "on" : "off");
	Chat_Add1("&e  offset: &f%f1 degrees", &fp.offset);
	Chat_Add1("&e  movement rotates with offset: &f%c", fp.rotateMove ? "yes" : "no");
}

static void FP_PrintHelp(void) {
	FP_Msg("&eFake player commands (&a/client fakeplayer ...&e):");
	FP_Msg("&aspawn [name] &7- spawn at your position (replaces the old one)");
	FP_Msg("&aremove &7- delete it    &atp &7- move it to you");
	FP_Msg("&aname <name> &7- rename (also changes the tab list)");
	FP_Msg("&amodel <file|humanoid> &7- .bbmodel path, or just a name from the &fmodels &7folder");
	FP_Msg("&askin <name|url> &7- skin for humanoid / texture-less models");
	FP_Msg("&amimic [on|off] &7- copy your movement and rotation");
	FP_Msg("&aoffset <degrees> &7- rotation offset while mimicking (180 = flipped)");
	FP_Msg("&amoverot [on|off] &7- rotate copied movement by the offset too");
}

static cc_bool FP_ParseOnOff(const cc_string* arg, cc_bool* value) {
	if (!arg->length) { *value = !*value; return true; }
	if (String_CaselessEqualsConst(arg, "on")  || String_CaselessEqualsConst(arg, "true")  || String_CaselessEqualsConst(arg, "1")) { *value = true;  return true; }
	if (String_CaselessEqualsConst(arg, "off") || String_CaselessEqualsConst(arg, "false") || String_CaselessEqualsConst(arg, "0")) { *value = false; return true; }
	return false;
}

static void FakePlayer_Execute(const cc_string* args, int argsCount) {
	cc_string sub, rest;
	struct Entity* e;
	cc_bool on;
	float v;

	if (!argsCount) { FP_PrintStatus(); return; }
	String_UNSAFE_Separate(args, ' ', &sub, &rest);
	/* allow multiple spaces between sub command and argument */
	while (rest.length && rest.buffer[0] == ' ') rest = String_UNSAFE_SubstringAt(&rest, 1);

	if (String_CaselessEqualsConst(&sub, "spawn") || String_CaselessEqualsConst(&sub, "create") || String_CaselessEqualsConst(&sub, "add")) {
		cc_string name = String_FromReadonly(FP_DEFAULT_NAME);
		if (rest.length) name = rest;
		FP_Spawn(&name);
		if (fp.active) Chat_Add1("&e/client: &fSpawned fake player &a%s&f at your position.", &fp.name);

	} else if (String_CaselessEqualsConst(&sub, "remove") || String_CaselessEqualsConst(&sub, "delete") || String_CaselessEqualsConst(&sub, "despawn")) {
		if (!FP_RequireActive()) return;
		FP_Remove();
		FP_Msg("&e/client: &fFake player removed.");

	} else if (String_CaselessEqualsConst(&sub, "name") || String_CaselessEqualsConst(&sub, "rename")) {
		if (!FP_RequireActive()) return;
		if (!rest.length) { FP_Msg("&e/client: &cUsage: &f/client fakeplayer name <name>"); return; }
		FP_ApplyName(&rest);
		Chat_Add1("&e/client: &fFake player is now called &a%s&f.", &fp.name);

	} else if (String_CaselessEqualsConst(&sub, "model")) {
		const char* err;
		if (!FP_RequireActive()) return;
		if (!rest.length) { FP_Msg("&e/client: &cUsage: &f/client fakeplayer model <path to .bbmodel | name in models folder | humanoid>"); return; }

		if (String_CaselessEqualsConst(&rest, "humanoid") || String_CaselessEqualsConst(&rest, "default")) {
			FP_UseHumanoid();
			FP_Msg("&e/client: &fBack to the normal humanoid model.");
			return;
		}
		err = FP_LoadBBModel(&rest);
		if (err) {
			Chat_Add2("&e/client: &cCouldn't load model '&f%s&c': %c", &rest, err);
			if (err[0] == 'n') FP_Msg("&7Checked the path you gave and the &fmodels &7folder next to the client.");
		} else {
			Chat_Add1("&e/client: &fFake player model set to &a%s&f.", &rest);
		}

	} else if (String_CaselessEqualsConst(&sub, "skin")) {
		if (!FP_RequireActive()) return;
		if (!rest.length) { FP_Msg("&e/client: &cUsage: &f/client fakeplayer skin <name|url>"); return; }
		if (fp.hasTex) { FP_Msg("&e/client: &cThat model has its own texture, so the skin isn't used."); return; }
		e = FP_Entity();
		if (e) Entity_SetSkin(e, &rest);
		FP_Msg("&e/client: &fSkin changed.");

	} else if (String_CaselessEqualsConst(&sub, "mimic")) {
		if (!FP_RequireActive()) return;
		on = fp.mimic;
		if (!FP_ParseOnOff(&rest, &on)) { FP_Msg("&e/client: &cUsage: &f/client fakeplayer mimic [on|off]"); return; }
		fp.mimic = on;
		if (on) {
			/* continue from wherever it is now */
			e = FP_Entity();
			if (e) fp.pos = e->next.pos;
		}
		FP_Msg(on ? "&e/client: &fFake player now mimics you." : "&e/client: &fFake player stopped mimicking.");

	} else if (String_CaselessEqualsConst(&sub, "offset") || String_CaselessEqualsConst(&sub, "rotation") || String_CaselessEqualsConst(&sub, "rot")) {
		if (!FP_RequireActive()) return;
		if (!Convert_ParseFloat(&rest, &v)) { FP_Msg("&e/client: &cUsage: &f/client fakeplayer offset <degrees>"); return; }
		fp.offset = v;
		Chat_Add1("&e/client: &fRotation offset is now &a%f1 &fdegrees.", &fp.offset);

	} else if (String_CaselessEqualsConst(&sub, "moverot")) {
		if (!FP_ParseOnOff(&rest, &fp.rotateMove)) { FP_Msg("&e/client: &cUsage: &f/client fakeplayer moverot [on|off]"); return; }
		FP_Msg(fp.rotateMove ? "&e/client: &fCopied movement is rotated by the offset." : "&e/client: &fCopied movement is NOT rotated by the offset.");

	} else if (String_CaselessEqualsConst(&sub, "tp") || String_CaselessEqualsConst(&sub, "here") || String_CaselessEqualsConst(&sub, "teleport")) {
		if (!FP_RequireActive()) return;
		FP_Teleport(&Entities.CurPlayer->Base);
		FP_Msg("&e/client: &fMoved the fake player to you.");

	} else if (String_CaselessEqualsConst(&sub, "status") || String_CaselessEqualsConst(&sub, "info")) {
		FP_PrintStatus();

	} else if (String_CaselessEqualsConst(&sub, "help")) {
		FP_PrintHelp();

	} else {
		FP_Msg("&e/client: &cUnknown fakeplayer option. Try &f/client fakeplayer help");
	}
}

struct ChatCommand FakePlayerCommand = {
	"FakePlayer", FakePlayer_Execute,
	COMMAND_FLAG_UNSPLIT_ARGS,
	{
		"&a/client fakeplayer <spawn|remove|name|model|skin|mimic|offset|tp|status|help>",
		"&eSpawns a client-side player at your position that also shows in the tab list.",
		"&e/client fakeplayer model <file.bbmodel> loads a Blockbench model (looks in",
		"&ethe path you give, then the &fmodels &efolder). &fmimic &ecopies your movement,",
		"&e&foffset <deg> &erotates it (180 = flipped). &fhelp &elists everything.",
	}
};
