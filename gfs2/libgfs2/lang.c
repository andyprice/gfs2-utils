#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/queue.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>

#include "parser.h"
#include "lang.h"

const char* ast_type_string[] = {
	[AST_NONE] = "NONE",
	// Statements
	[AST_ST_SET] = "SET",
	[AST_ST_GET] = "GET",

	// Expressions
	[AST_EX_ID] = "IDENTIFIER",
	[AST_EX_NUMBER] = "NUMBER",
	[AST_EX_STRING] = "STRING",
	[AST_EX_ADDRESS] = "ADDRESS",
	[AST_EX_PATH] = "PATH",
	[AST_EX_SUBSCRIPT] = "SUBSCRIPT",
	[AST_EX_OFFSET] = "OFFSET",
	[AST_EX_BLOCKSPEC] = "BLOCKSPEC",
	[AST_EX_STRUCTSPEC] = "STRUCTSPEC",
	[AST_EX_FIELDSPEC] = "FIELDSPEC",

	// Keywords
	[AST_KW_STATE] = "STATE",
};

/**
 * Initialize an expression node of the given type from a source string.
 * Currently just converts numerical values and string values where
 * appropriate. String values are duplicted into newly allocated buffers as the
 * text from the parser will go away.
 * Returns 0 on success or non-zero with errno set on failure
 */
static int ast_expr_init(struct ast_node *expr, ast_node_t type, const char *str)
{
	int ret = 0;
	switch (type) {
	case AST_EX_OFFSET:
		str++; // Cut off the +
	case AST_EX_NUMBER:
		ret = sscanf(str, "%"SCNi64, &expr->ast_num);
		if (ret != 1) {
			return 1;
		}
		break;
	case AST_EX_ID:
	case AST_EX_PATH:
	case AST_EX_STRING:
		expr->ast_str = strdup(str);
		if (expr->ast_str == NULL) {
			return 1;
		}
		break;
	case AST_EX_ADDRESS:
	case AST_EX_SUBSCRIPT:
	case AST_EX_BLOCKSPEC:
	case AST_EX_STRUCTSPEC:
	case AST_EX_FIELDSPEC:
	case AST_KW_STATE:
		break;
	default:
		errno = EINVAL;
		return 1;
	}
	return 0;
}

/**
 * Create a new AST node of a given type from a source string.
 * Returns a pointer to the new node or NULL on failure with errno set.
 */
struct ast_node *ast_new(ast_node_t type, const char *text)
{
	struct ast_node *node;
	node = (struct ast_node *)calloc(1, sizeof(struct ast_node));
	if (node == NULL) {
		goto return_fail;
	}

	if (type > _AST_EX_START && ast_expr_init(node, type, text)) {
		goto return_free;
	}

	node->ast_text = strdup(text);
	if (node->ast_text == NULL) {
		goto return_free;
	}
	node->ast_type = type;

	return node;

return_free:
	if (node->ast_text) {
		free(node->ast_text);
	}
	if (node->ast_str) {
		free(node->ast_str);
	}
	free(node);
return_fail:
	fprintf(stderr, "Failed to create new value from %s: %s\n", text, strerror(errno));
	return NULL;
}

/**
 * Free the memory allocated for an AST node and set its pointer to NULL
 */
void ast_destroy(struct ast_node **node)
{
	if (*node == NULL) {
		return;
	}
	ast_destroy(&(*node)->ast_left);
	ast_destroy(&(*node)->ast_right);
	switch((*node)->ast_type) {
	case AST_EX_ID:
	case AST_EX_PATH:
	case AST_EX_STRING:
		free((*node)->ast_str);
		break;
	default:
		break;
	}
	free((*node)->ast_text);
	free(*node);
	*node = NULL;
}

static void ast_string_unescape(char *str)
{
	int head, tail;
	for (head = tail = 0; str[head] != '\0'; head++, tail++) {
		if (str[head] == '\\' && str[head+1] != '\0')
			head++;
		str[tail] = str[head];
	}
	str[tail] = '\0';
}

static uint64_t ast_lookup_path(char *path, struct gfs2_sbd *sbd)
{
	int err = 0;
	char *c;
	struct gfs2_inode *ip, *iptmp;
	char *segment;
	uint64_t bn = 0;

	segment = strtok_r(path, "/", &c);
	ip = lgfs2_inode_read(sbd, sbd->sd_sb.sb_root_dir.no_addr);

	while (ip != NULL) {
		if (segment == NULL) { // No more segments
			bn = ip->i_di.di_num.no_addr;
			inode_put(&ip);
			return bn;
		}
		ast_string_unescape(segment);
		err = gfs2_lookupi(ip, segment, strlen(segment), &iptmp);
		inode_put(&ip);
		if (err != 0) {
			errno = -err;
			break;
		}
		ip = iptmp;
		segment = strtok_r(NULL, "/", &c);
	}

	perror("Path lookup");
	return 0;
}

enum block_id {
	ID_SB	= 0,
	ID_MASTER,
	ID_ROOT,
	ID_RINDEX,

	ID_END
};

/**
 * Names of blocks which can be uniquely identified in the fs
 */
static const char *block_ids[] = {
	[ID_SB]		= "sb",
	[ID_MASTER]	= "master",
	[ID_ROOT]	= "root",
	[ID_RINDEX]	= "rindex",

	[ID_END]	= NULL
};

static uint64_t ast_lookup_id(const char *id, struct gfs2_sbd *sbd)
{
	uint64_t bn = 0;
	int i;
	for (i = 0; i < ID_END; i++) {
		if (!strcmp(id, block_ids[i])) {
			break;
		}
	}
	switch (i) {
	case ID_SB:
		bn = sbd->sb_addr;
		break;
	case ID_MASTER:
		bn = sbd->sd_sb.sb_master_dir.no_addr;
		break;
	case ID_ROOT:
		bn = sbd->sd_sb.sb_root_dir.no_addr;
		break;
	case ID_RINDEX:
		bn = sbd->md.riinode->i_di.di_num.no_addr;
		break;
	default:
		return 0;
	}
	return bn;
}

static uint64_t ast_lookup_rgrp(uint64_t rgnum, struct gfs2_sbd *sbd)
{
	uint64_t i = rgnum;
	struct osi_node *n;

	for (n = osi_first(&sbd->rgtree); n != NULL && i > 0; n = osi_next(n), i--);
	if (n != NULL && i == 0)
		return ((struct rgrp_tree *)n)->ri.ri_addr;
	fprintf(stderr, "Resource group number out of range: %"PRIu64"\n", rgnum);
	return 0;
}

static uint64_t ast_lookup_subscript(struct ast_node *id, struct ast_node *index,
                                     struct gfs2_sbd *sbd)
{
	uint64_t bn = 0;
	const char *name = id->ast_str;
	if (!strcmp(name, "rgrp")) {
		bn = ast_lookup_rgrp(index->ast_num, sbd);
	} else {
		fprintf(stderr, "Unrecognized identifier %s\n", name);
	}
	return bn;
}

/**
 * Look up a block and return its number. The kind of lookup depends on the
 * type of the ast node.
 */
static uint64_t ast_lookup_block_num(struct ast_node *ast, struct gfs2_sbd *sbd)
{
	uint64_t bn = 0;
	switch (ast->ast_type) {
	case AST_EX_OFFSET:
		bn = ast_lookup_block_num(ast->ast_left, sbd) + ast->ast_num;
		break;
	case AST_EX_ADDRESS:
		bn = ast->ast_num;
		break;
	case AST_EX_PATH:
		bn = ast_lookup_path(ast->ast_str, sbd);
		break;
	case AST_EX_ID:
		bn = ast_lookup_id(ast->ast_str, sbd);
		break;
	case AST_EX_SUBSCRIPT:
		bn = ast_lookup_subscript(ast->ast_left, ast->ast_left->ast_left, sbd);
		break;
	default:
		break;
	}
	return bn;
}

static struct gfs2_buffer_head *ast_lookup_block(struct ast_node *node, struct gfs2_sbd *sbd)
{
	uint64_t bn = ast_lookup_block_num(node, sbd);
	if (bn == 0) {
		return NULL;
	}

	return bread(sbd, bn);
}

static const char *bitstate_strings[] = {
	[GFS2_BLKST_FREE] = "Free",
	[GFS2_BLKST_USED] = "Used",
	[GFS2_BLKST_UNLINKED] = "Unlinked",
	[GFS2_BLKST_DINODE] = "Dinode"
};

/**
 * Print a representation of an arbitrary GFS2 block to stdout
 */
int lgfs2_lang_result_print(struct lgfs2_lang_result *result)
{
	int i;
	if (result->lr_mtype != NULL) {
		for (i = 0; i < result->lr_mtype->nfields; i++) {
			lgfs2_field_print(result->lr_bh, result->lr_mtype, &result->lr_mtype->fields[i]);
		}
	} else {
		printf("%"PRIu64": %s\n", result->lr_blocknr, bitstate_strings[result->lr_state]);
	}
	return 0;
}

static int ast_get_bitstate(uint64_t bn, struct gfs2_sbd *sbd)
{
	int ret = 0;
	int state = 0;
	struct rgrp_tree *rgd = gfs2_blk2rgrpd(sbd, bn);
	if (rgd == NULL) {
		fprintf(stderr, "Could not find resource group for block %"PRIu64"\n", bn);
		return -1;
	}

	ret = gfs2_rgrp_read(sbd, rgd);
	if (ret != 0) {
		fprintf(stderr, "Failed to read resource group for block %"PRIu64": %d\n", bn, ret);
		return -1;
	}

	state = lgfs2_get_bitmap(sbd, bn, rgd);
	if (state == -1) {
		fprintf(stderr, "Failed to acquire bitmap state for block %"PRIu64"\n", bn);
		return -1;
	}

	gfs2_rgrp_relse(rgd);
	return state;
}

static const struct lgfs2_metadata *ast_lookup_mtype(const struct gfs2_buffer_head *bh)
{
	const struct lgfs2_metadata *mtype;
	const uint32_t mh_type = lgfs2_get_block_type(bh);
	if (mh_type == 0) {
		fprintf(stderr, "Could not determine type for block %"PRIu64"\n", bh->b_blocknr);
		return NULL;
	}

	mtype = lgfs2_find_mtype(mh_type, bh->sdp->gfs1 ? LGFS2_MD_GFS1 : LGFS2_MD_GFS2);
	if (mtype == NULL) {
		fprintf(stderr, "Could not determine meta type for block %"PRIu64"\n", bh->b_blocknr);
		return NULL;
	}
	return mtype;
}

/**
 * Interpret the get statement.
 */
static struct lgfs2_lang_result *ast_interp_get(struct lgfs2_lang_state *state,
                                     struct ast_node *ast, struct gfs2_sbd *sbd)
{
	struct lgfs2_lang_result *result = calloc(1, sizeof(struct lgfs2_lang_result));
	if (result == NULL) {
		fprintf(stderr, "Failed to allocate memory for result\n");
		return NULL;
	}

	if (ast->ast_right->ast_right == NULL) {
		result->lr_bh = ast_lookup_block(ast->ast_right, sbd);
		if (result->lr_bh == NULL) {
			free(result);
			return NULL;
		}
		result->lr_blocknr = result->lr_bh->b_blocknr;
		result->lr_mtype = ast_lookup_mtype(result->lr_bh);

	} else if (ast->ast_right->ast_right->ast_type == AST_KW_STATE) {
		result->lr_blocknr = ast_lookup_block_num(ast->ast_right, sbd);
		if (result->lr_blocknr == 0) {
			return NULL;
		}
		result->lr_state = ast_get_bitstate(result->lr_blocknr, sbd);
	}

	return result;
}

/**
 * Interpret a UUID string by removing hyphens from the string and then
 * interprets 16 pairs of hex digits as octets.
 */
static int ast_str_to_uuid(const char *str, uint8_t *uuid)
{
	char s[33];
	int head, tail, tmp;

	for (head = tail = 0; head < strlen(str) && tail < 33; head++) {
		if (str[head] == '-')
			continue;
		s[tail] = tolower(str[head]);
		if (!((s[tail] >= 'a' && s[tail] <= 'f') ||
		      (s[tail] >= '0' && s[tail] <= '9')))
			goto invalid;
		tail++;
	}
	if (tail != 32) {
		goto invalid;
	}
	s[tail] = '\0';
	for (head = 0; head < 16; head++) {
		if (sscanf(s+(head*2), "%02x", &tmp) != 1) {
			goto invalid;
		}
		*(uuid + head) = tmp;
	}
	return AST_INTERP_SUCCESS;
invalid:
	fprintf(stderr, "Invalid UUID\n");
	return AST_INTERP_INVAL;
}

/**
 * Set a field of a gfs2 block of a given type to a given value.
 * Returns AST_INTERP_* to signal success, an invalid field/value or an error.
 */
static int ast_field_set(struct gfs2_buffer_head *bh, const struct lgfs2_metafield *field,
                                                                        struct ast_node *val)
{
	char *fieldp = (char *)bh->iov.iov_base + field->offset;

	if (field->flags & LGFS2_MFF_UUID) {
		uint8_t uuid[16];
		int ret = ast_str_to_uuid(val->ast_str, uuid);

		if (ret != AST_INTERP_SUCCESS)
			return ret;

		memcpy(fieldp, uuid, 16);
		bmodified(bh);
		return AST_INTERP_SUCCESS;
	}

	if ((field->flags & LGFS2_MFF_STRING) && strlen(val->ast_str) > field->length) {
		fprintf(stderr, "String '%s' is too long for field '%s'\n", val->ast_str, field->name);
		return AST_INTERP_INVAL;
	}

	if (field->flags & (LGFS2_MFF_STRING|LGFS2_MFF_UUID)) {
		strncpy(fieldp, val->ast_str, field->length - 1);
		fieldp[field->length - 1] = '\0';
		bmodified(bh);
		return AST_INTERP_SUCCESS;
	} else {
		// Numeric fields
		switch(field->length) {
		case 1:
			if (val->ast_num > UINT8_MAX)
				break;
			*fieldp = (uint8_t)val->ast_num;
			bmodified(bh);
			return AST_INTERP_SUCCESS;
		case 2:
			if (val->ast_num > UINT16_MAX)
				break;
			*(uint16_t *)fieldp = cpu_to_be16((uint16_t)val->ast_num);
			bmodified(bh);
			return AST_INTERP_SUCCESS;
		case 4:
			if (val->ast_num > UINT32_MAX)
				break;
			*(uint32_t *)fieldp = cpu_to_be32((uint32_t)val->ast_num);
			bmodified(bh);
			return AST_INTERP_SUCCESS;
		case 8:
			*(uint64_t *)fieldp = cpu_to_be64((uint64_t)val->ast_num);
			bmodified(bh);
			return AST_INTERP_SUCCESS;
		default:
			// This should never happen
			return AST_INTERP_ERR;
		}
	}

	fprintf(stderr, "Invalid field assignment: %s (size %d) = %s\n",
	                     field->name, field->length, val->ast_text);
	return AST_INTERP_INVAL;
}

/**
 * Interpret an assignment (set)
 */
static struct lgfs2_lang_result *ast_interp_set(struct lgfs2_lang_state *state,
                                    struct ast_node *ast, struct gfs2_sbd *sbd)
{
	struct ast_node *lookup = ast->ast_right;
	struct ast_node *fieldspec;
	struct ast_node *fieldname;
	struct ast_node *fieldval;
	uint32_t mh_type = 0;
	int i = 0;
	int ret = 0;

	struct lgfs2_lang_result *result = calloc(1, sizeof(struct lgfs2_lang_result));
	if (result == NULL) {
		fprintf(stderr, "Failed to allocate memory for result\n");
		return NULL;
	}

	result->lr_bh = ast_lookup_block(lookup, sbd);
	if (result->lr_bh == NULL) {
		goto out_err;
	}

	mh_type = lgfs2_get_block_type(result->lr_bh);
	if (mh_type == 0) {
		goto out_err;
	}

	result->lr_mtype = lgfs2_find_mtype(mh_type, sbd->gfs1 ? LGFS2_MD_GFS1 : LGFS2_MD_GFS2);
	if (result->lr_mtype == NULL) {
		goto out_err;
	}

	for (fieldspec = lookup->ast_right;
	     fieldspec != NULL && fieldspec->ast_type == AST_EX_FIELDSPEC;
	     fieldspec = fieldspec->ast_left) {

		fieldname = fieldspec->ast_right;
		fieldval = fieldname->ast_right;
		for (i = 0; i < result->lr_mtype->nfields; i++) {
			if (!strcmp(result->lr_mtype->fields[i].name, fieldname->ast_str)) {
				ret = ast_field_set(result->lr_bh, &result->lr_mtype->fields[i], fieldval);
				if (ret != AST_INTERP_SUCCESS) {
					goto out_err;
				}
				break;
			}
		}
	}

	ret = bwrite(result->lr_bh);
	if (ret != 0) {
		fprintf(stderr, "Failed to write modified block %"PRIu64": %s\n",
		                        result->lr_bh->b_blocknr, strerror(errno));
		goto out_err;
	}

	return result;

out_err:
	lgfs2_lang_result_free(&result);
	return NULL;
}

static struct lgfs2_lang_result *ast_interpret_node(struct lgfs2_lang_state *state,
                                        struct ast_node *ast, struct gfs2_sbd *sbd)
{
	struct lgfs2_lang_result *result = NULL;

	if (ast->ast_type == AST_ST_SET) {
		result = ast_interp_set(state, ast, sbd);
	} else if (ast->ast_type == AST_ST_GET) {
		result = ast_interp_get(state, ast, sbd);
	} else {
		fprintf(stderr, "Invalid AST node type: %d\n", ast->ast_type);
	}
	return result;
}

struct lgfs2_lang_result *lgfs2_lang_result_next(struct lgfs2_lang_state *state,
                                                           struct gfs2_sbd *sbd)
{
	struct lgfs2_lang_result *result;
	if (state->ls_interp_curr == NULL) {
		return NULL;
	}
	result = ast_interpret_node(state, state->ls_interp_curr, sbd);
	if (result == NULL) {
		return NULL;
	}
	state->ls_interp_curr = state->ls_interp_curr->ast_left;
	return result;
}

void lgfs2_lang_result_free(struct lgfs2_lang_result **result)
{
	if (*result == NULL) {
		fprintf(stderr, "Warning: attempted to free a null result\n");
		return;
	}

	if ((*result)->lr_mtype != NULL) {
		(*result)->lr_bh->b_modified = 0;
		brelse((*result)->lr_bh);
		(*result)->lr_bh = NULL;
	}

	free(*result);
	*result = NULL;
}