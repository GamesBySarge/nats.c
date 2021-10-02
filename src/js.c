// Copyright 2021 The NATS Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <ctype.h>

#include "js.h"
#include "mem.h"
#include "conn.h"
#include "util.h"
#include "opts.h"
#include "sub.h"

#ifdef DEV_MODE
// For type safety

void js_lock(jsCtx *js)   { natsMutex_Lock(js->mu);   }
void js_unlock(jsCtx *js) { natsMutex_Unlock(js->mu); }

static void _retain(jsCtx *js)  { js->refs++; }
static void _release(jsCtx *js) { js->refs--; }

#else

#define _retain(js)         ((js)->refs++)
#define _release(js)        ((js)->refs--)

#endif // DEV_MODE


const char*      jsDefaultAPIPrefix      = "$JS.API";
const int64_t    jsDefaultRequestWait    = 5000;
const int64_t    jsDefaultStallWait      = 200;
const char       *jsDigits               = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
const int        jsBase                  = 62;

#define jsReplyTokenSize    (8)
#define jsReplyPrefixLen    (NATS_INBOX_PRE_LEN + (jsReplyTokenSize) + 1)
#define jsDefaultMaxMsgs    (512 * 1024)

#define jsAckPrefix             "$JS.ACK."
#define jsAckPrefixLen          (8)
#define jsLastConsumerSeqHdr    "Nats-Last-Consumer"

static void
_destroyOptions(jsOptions *o)
{
    NATS_FREE((char*) o->Prefix);
    NATS_FREE((char*) o->Stream.Purge.Subject);
}

static void
_freeContext(jsCtx *js)
{
    natsConnection *nc = NULL;

    natsStrHash_Destroy(js->pm);
    natsSubscription_Destroy(js->rsub);
    _destroyOptions(&(js->opts));
    NATS_FREE(js->rpre);
    natsCondition_Destroy(js->cond);
    natsMutex_Destroy(js->mu);
    nc = js->nc;
    NATS_FREE(js);

    natsConn_release(nc);
}

void
js_retain(jsCtx *js)
{
    js_lock(js);
    js->refs++;
    js_unlock(js);
}

void
js_release(jsCtx *js)
{
    bool doFree;

    js_lock(js);
    doFree = (--(js->refs) == 0);
    js_unlock(js);

    if (doFree)
        _freeContext(js);
}

static void
js_unlockAndRelease(jsCtx *js)
{
    bool doFree;

    doFree = (--(js->refs) == 0);
    js_unlock(js);

    if (doFree)
        _freeContext(js);
}

void
jsCtx_Destroy(jsCtx *js)
{
    if (js == NULL)
        return;

    js_lock(js);
    if (js->rsub != NULL)
    {
        natsSubscription_Destroy(js->rsub);
        js->rsub = NULL;
    }
    if ((js->pm != NULL) && natsStrHash_Count(js->pm) > 0)
    {
        natsStrHashIter iter;
        void            *v = NULL;

        natsStrHashIter_Init(&iter, js->pm);
        while (natsStrHashIter_Next(&iter, NULL, &v))
        {
            natsMsg *msg = (natsMsg*) v;
            natsStrHashIter_RemoveCurrent(&iter);
            natsMsg_Destroy(msg);
        }
    }
    js_unlockAndRelease(js);
}

natsStatus
jsOptions_Init(jsOptions *opts)
{
    if (opts == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);

    memset(opts, 0, sizeof(jsOptions));
    return NATS_OK;
}

// Parse the JSON represented by the NATS message's payload and returns the JSON object.
// Unmarshal the API response.
natsStatus
js_unmarshalResponse(jsApiResponse *ar, nats_JSON **new_json, natsMsg *resp)
{
    nats_JSON   *json = NULL;
    nats_JSON   *err  = NULL;
    natsStatus  s;

    memset(ar, 0, sizeof(jsApiResponse));

    s = nats_JSONParse(&json, natsMsg_GetData(resp), natsMsg_GetDataLength(resp));
    if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);

    // Check if there is an "error" field.
    s = nats_JSONGetObject(json, "error", &err);
    if ((s == NATS_OK) && (err != NULL))
    {
        s = nats_JSONGetInt(err, "code", &(ar->Error.Code));
        IFOK(s, nats_JSONGetUInt16(err, "err_code", &(ar->Error.ErrCode)));
        IFOK(s, nats_JSONGetStr(err, "description", &(ar->Error.Description)));
    }

    if (s == NATS_OK)
        *new_json = json;
    else
        nats_JSONDestroy(json);

    return NATS_UPDATE_ERR_STACK(s);
}

void
js_freeApiRespContent(jsApiResponse *ar)
{
    if (ar == NULL)
        return;

    NATS_FREE(ar->Type);
    NATS_FREE(ar->Error.Description);
}

static natsStatus
_copyPurgeOptions(jsCtx *js, struct jsOptionsStreamPurge *o)
{
    natsStatus                      s   = NATS_OK;
    struct jsOptionsStreamPurge *po = &(js->opts.Stream.Purge);

    po->Sequence = o->Sequence;
    po->Keep     = o->Keep;

    if (!nats_IsStringEmpty(o->Subject))
    {
        po->Subject = NATS_STRDUP(o->Subject);
        if (po->Subject == NULL)
            s = nats_setDefaultError(NATS_NO_MEMORY);
    }

    return NATS_UPDATE_ERR_STACK(s);
}

static natsStatus
_copyStreamInfoOptions(jsCtx *js, struct jsOptionsStreamInfo *o)
{
    js->opts.Stream.Info.DeletedDetails = o->DeletedDetails;
    return NATS_OK;
}

natsStatus
natsConnection_JetStream(jsCtx **new_js, natsConnection *nc, jsOptions *opts)
{
    jsCtx       *js = NULL;
    natsStatus  s;

    if ((new_js == NULL) || (nc == NULL))
        return nats_setDefaultError(NATS_INVALID_ARG);

    if (opts != NULL)
    {
        if (opts->Wait < 0)
            return nats_setError(NATS_INVALID_ARG, "option 'Wait' (%" PRId64 ") cannot be negative", opts->Wait);
        if (opts->PublishAsync.StallWait < 0)
            return nats_setError(NATS_INVALID_ARG, "option 'PublishAsyncStallWait' (%" PRId64 ") cannot be negative", opts->PublishAsync.StallWait);
    }

    js = (jsCtx*) NATS_CALLOC(1, sizeof(jsCtx));
    if (js == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    js->refs = 1;
    // Retain the NATS connection and keep track of it so that if we
    // detroy the context, in case of failure to fully initialize,
    // we properly release the NATS connection.
    natsConn_retain(nc);
    js->nc = nc;

    s = natsMutex_Create(&(js->mu));
    if (s == NATS_OK)
    {
        // If Domain is set, use domain to create prefix.
        if ((opts != NULL) && !nats_IsStringEmpty(opts->Domain))
        {
            if (nats_asprintf((char**) &(js->opts.Prefix), "$JS.%.*s.API",
                js_lenWithoutTrailingDot(opts->Domain), opts->Domain) < 0)
            {
                s = nats_setDefaultError(NATS_NO_MEMORY);
            }
        }
        else if ((opts == NULL) || nats_IsStringEmpty(opts->Prefix))
        {
            js->opts.Prefix = NATS_STRDUP(jsDefaultAPIPrefix);
            if (js->opts.Prefix == NULL)
                s = nats_setDefaultError(NATS_NO_MEMORY);
        }
        else if (nats_asprintf((char**) &(js->opts.Prefix), "%.*s",
                js_lenWithoutTrailingDot(opts->Prefix), opts->Prefix) < 0)
        {
                s = nats_setDefaultError(NATS_NO_MEMORY);
        }
    }
    if ((s == NATS_OK) && (opts != NULL))
    {
        struct jsOptionsPublishAsync *pa = &(js->opts.PublishAsync);

        pa->MaxPending          = opts->PublishAsync.MaxPending;
        pa->ErrHandler          = opts->PublishAsync.ErrHandler;
        pa->ErrHandlerClosure   = opts->PublishAsync.ErrHandlerClosure;
        pa->StallWait           = opts->PublishAsync.StallWait;
        js->opts.Wait           = opts->Wait;
    }
    if (js->opts.Wait == 0)
        js->opts.Wait = jsDefaultRequestWait;
    if (js->opts.PublishAsync.StallWait == 0)
        js->opts.PublishAsync.StallWait = jsDefaultStallWait;
    if ((s == NATS_OK) && (opts != NULL))
    {
        s = _copyPurgeOptions(js, &(opts->Stream.Purge));
        IFOK(s, _copyStreamInfoOptions(js, &(opts->Stream.Info)));
    }

    if (s == NATS_OK)
        *new_js = js;
    else
        jsCtx_Destroy(js);

    return NATS_UPDATE_ERR_STACK(s);
}

int
js_lenWithoutTrailingDot(const char *str)
{
    int l = (int) strlen(str);

    if (str[l-1] == '.')
        l--;
    return l;
}

natsStatus
js_setOpts(natsConnection **nc, bool *freePfx, jsCtx *js, jsOptions *opts, jsOptions *resOpts)
{
    natsStatus s = NATS_OK;

    *freePfx = false;
    jsOptions_Init(resOpts);

    if ((opts != NULL) && !nats_IsStringEmpty(opts->Domain))
    {
        char *pfx = NULL;
        if (nats_asprintf(&pfx, "$JS.%.*s.API",
                js_lenWithoutTrailingDot(opts->Domain), opts->Domain) < 0)
        {
            s = nats_setDefaultError(NATS_NO_MEMORY);
        }
        else
        {
            resOpts->Prefix = pfx;
            *freePfx        = true;
        }
    }
    if (s == NATS_OK)
    {
        struct jsOptionsStreamPurge *po = &(js->opts.Stream.Purge);

        js_lock(js);
        // If not set above...
        if (resOpts->Prefix == NULL)
            resOpts->Prefix = (opts == NULL || nats_IsStringEmpty(opts->Prefix)) ? js->opts.Prefix : opts->Prefix;

        // Take provided one or default to context's.
        resOpts->Wait = (opts == NULL || opts->Wait <= 0) ? js->opts.Wait : opts->Wait;

        // Purge options
        if (opts != NULL)
        {
            struct jsOptionsStreamPurge *opo = &(opts->Stream.Purge);

            // If any field is set, use `opts`, otherwise, we will use the
            // context's purge options.
            if ((opo->Subject != NULL) || (opo->Sequence > 0) || (opo->Keep > 0))
                po = opo;
        }
        memcpy(&(resOpts->Stream.Purge), po, sizeof(*po));

        // Stream info options
        resOpts->Stream.Info.DeletedDetails = (opts == NULL ? js->opts.Stream.Info.DeletedDetails : opts->Stream.Info.DeletedDetails);

        *nc = js->nc;
        js_unlock(js);
    }
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
jsPubOptions_Init(jsPubOptions *opts)
{
    if (opts == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);

    memset(opts, 0, sizeof(jsPubOptions));
    return NATS_OK;
}

natsStatus
js_Publish(jsPubAck **new_puback, jsCtx *js, const char *subj, const void *data, int dataLen,
           jsPubOptions *opts, jsErrCode *errCode)
{
    natsStatus s;
    natsMsg    msg;

    natsMsg_init(&msg, subj, (const char*) data, dataLen);
    s = js_PublishMsg(new_puback, js, &msg, opts, errCode);
    natsMsg_freeHeaders(&msg);

    return NATS_UPDATE_ERR_STACK(s);
}

static natsStatus
_setHeadersFromOptions(natsMsg *msg, jsPubOptions *opts)
{
    natsStatus  s        = NATS_OK;
    char        temp[64] = {'\0'};

    if (!nats_IsStringEmpty(opts->MsgId))
        s = natsMsgHeader_Set(msg, jsMsgIdHdr, opts->MsgId);

    if ((s == NATS_OK) && !nats_IsStringEmpty(opts->ExpectLastMsgId))
        s = natsMsgHeader_Set(msg, jsExpectedLastMsgIdHdr, opts->ExpectLastMsgId);

    if ((s == NATS_OK) && !nats_IsStringEmpty(opts->ExpectStream))
        s = natsMsgHeader_Set(msg, jsExpectedStreamHdr, opts->ExpectStream);

    if ((s == NATS_OK) && (opts->ExpectLastSeq > 0))
    {
        snprintf(temp, sizeof(temp), "%" PRIu64, opts->ExpectLastSeq);
        s = natsMsgHeader_Set(msg, jsExpectedLastSeqHdr, temp);
    }

    if ((s == NATS_OK) && (opts->ExpectLastSubjectSeq > 0))
    {
        snprintf(temp, sizeof(temp), "%" PRIu64, opts->ExpectLastSubjectSeq);
        s = natsMsgHeader_Set(msg, jsExpectedLastSubjSeqHdr, temp);
    }

    return NATS_UPDATE_ERR_STACK(s);
}

static natsStatus
_checkMaxWaitOpt(int64_t *new_ttl, jsPubOptions *opts)
{
    int64_t ttl;

    if ((ttl = opts->MaxWait) < 0)
        return nats_setError(NATS_INVALID_ARG, "option 'MaxWait' (%" PRId64 ") cannot be negative", ttl);

    *new_ttl = ttl;
    return NATS_OK;
}

natsStatus
js_PublishMsg(jsPubAck **new_puback,jsCtx *js, natsMsg *msg,
              jsPubOptions *opts, jsErrCode *errCode)
{
    natsStatus          s       = NATS_OK;
    int64_t             ttl     = 0;
    nats_JSON           *json   = NULL;
    natsMsg             *resp   = NULL;
    jsApiResponse   ar;

    if (errCode != NULL)
        *errCode = 0;

    if ((js == NULL) || (msg == NULL) || nats_IsStringEmpty(msg->subject))
        return nats_setDefaultError(NATS_INVALID_ARG);

    if (opts != NULL)
    {
        s = _checkMaxWaitOpt(&ttl, opts);
        IFOK(s, _setHeadersFromOptions(msg, opts));
        if (s != NATS_OK)
            return NATS_UPDATE_ERR_STACK(s);
    }

    // As it would be for a NATS connection, if the context has been destroyed,
    // the memory is invalid and accessing any field of the context could cause
    // a SEGFAULT. But assuming the context is still valid, we can access its
    // options and the NATS connection without locking since they are immutable
    // and the NATS connection has been retained when getting the JS context.

    // If not set through options, default to the context's Wait value.
    if (ttl == 0)
        ttl = js->opts.Wait;

    IFOK_JSR(s, natsConnection_RequestMsg(&resp, js->nc, msg, ttl));
    if (s == NATS_OK)
        s = js_unmarshalResponse(&ar, &json, resp);
    if (s == NATS_OK)
    {
        if (js_apiResponseIsErr(&ar))
        {
             if (errCode != NULL)
                *errCode = (int) ar.Error.ErrCode;
            s = nats_setError(NATS_ERR, "%s", ar.Error.Description);
        }
        else if (new_puback != NULL)
        {
            // The user wants the jsPubAck object back, so we need to unmarshal it.
            jsPubAck *pa = NULL;

            pa = (jsPubAck*) NATS_CALLOC(1, sizeof(jsPubAck));
            if (pa == NULL)
                s = nats_setDefaultError(NATS_NO_MEMORY);
            else
            {
                s = nats_JSONGetStr(json, "stream", &(pa->Stream));
                IFOK(s, nats_JSONGetULong(json, "seq", &(pa->Sequence)));
                IFOK(s, nats_JSONGetBool(json, "duplicate", &(pa->Duplicate)));
                IFOK(s, nats_JSONGetStr(json, "domain", &(pa->Domain)));

                if (s == NATS_OK)
                    *new_puback = pa;
                else
                    jsPubAck_Destroy(pa);
            }
        }
        js_freeApiRespContent(&ar);
        nats_JSONDestroy(json);
    }
    natsMsg_Destroy(resp);
    return NATS_UPDATE_ERR_STACK(s);
}

void
jsPubAck_Destroy(jsPubAck *pa)
{
    if (pa == NULL)
        return;

    NATS_FREE(pa->Stream);
    NATS_FREE(pa->Domain);
    NATS_FREE(pa);
}

static void
_handleAsyncReply(natsConnection *nc, natsSubscription *sub, natsMsg *msg, void *closure)
{
    const char      *subject    = natsMsg_GetSubject(msg);
    char            *id         = NULL;
    jsCtx           *js         = NULL;
    natsMsg         *pmsg       = NULL;
    char            errTxt[256] = {'\0'};
    jsPubAckErr     pae;
    struct jsOptionsPublishAsync *opa = NULL;

    if ((subject == NULL) || (int) strlen(subject) <= jsReplyPrefixLen)
    {
        natsMsg_Destroy(msg);
        return;
    }

    id = (char*) (subject+jsReplyPrefixLen);
    js = (jsCtx*) closure;

    js_lock(js);

    pmsg = natsStrHash_Remove(js->pm, id);
    if (pmsg == NULL)
    {
        natsMsg_Destroy(msg);
        js_unlock(js);
        return;
    }

    opa = &(js->opts.PublishAsync);
    if (opa->ErrHandler != NULL)
    {
        natsStatus s = NATS_OK;

        memset(&pae, 0, sizeof(jsPubAckErr));

        // Check for no responders
        if (natsMsg_IsNoResponders(msg))
        {
            s = NATS_NO_RESPONDERS;
        }
        else
        {
            nats_JSON           *json = NULL;
            jsApiResponse       ar;

            // Now unmarshal the API response and check if there was an error.

            s = js_unmarshalResponse(&ar, &json, msg);
            if ((s == NATS_OK) && js_apiResponseIsErr(&ar))
            {
                pae.Err     = NATS_ERR;
                pae.ErrCode = (int) ar.Error.ErrCode;
                snprintf(errTxt, sizeof(errTxt), "%s", ar.Error.Description);
            }
            js_freeApiRespContent(&ar);
            nats_JSONDestroy(json);
        }
        if (s != NATS_OK)
        {
            pae.Err = s;
            snprintf(errTxt, sizeof(errTxt), "%s", natsStatus_GetText(pae.Err));
        }

        // We will invoke CB only if there is any kind of error.
        if (pae.Err != NATS_OK)
        {
            // Associate the message with the pubAckErr object.
            pae.Msg = pmsg;
            // And the error text.
            pae.ErrText = errTxt;
            js_unlock(js);

            (opa->ErrHandler)(js, &pae, opa->ErrHandlerClosure);

            js_lock(js);

            // If the user resent the message, pae->Msg will have been cleared.
            // In this case, do not destroy the message. Do not blindly destroy
            // an address that could have been set, so destroy only if pmsg
            // is same value than pae->Msg.
            if (pae.Msg != pmsg)
                pmsg = NULL;
        }
    }

    // Now that the callback has returned, decrement the number of pending messages.
    js->pmcount--;

    // If there are callers waiting for async pub completion, or stalled async
    // publish calls and we are now below max pending, broadcast to unblock them.
    if (((js->pacw > 0) && (js->pmcount == 0))
        || ((js->stalled > 0) && (js->pmcount <= opa->MaxPending)))
    {
        natsCondition_Broadcast(js->cond);
    }
    js_unlock(js);

    natsMsg_Destroy(pmsg);
    natsMsg_Destroy(msg);
}

static void
_subComplete(void *closure)
{
    js_release((jsCtx*) closure);
}

static natsStatus
_newAsyncReply(char *reply, jsCtx *js)
{
    natsStatus  s           = NATS_OK;

    // Create the internal objects if it is the first time that we are doing
    // an async publish.
    if (js->rsub == NULL)
    {
        s = natsCondition_Create(&(js->cond));
        IFOK(s, natsStrHash_Create(&(js->pm), 64));
        if (s == NATS_OK)
        {
            js->rpre = NATS_MALLOC(jsReplyPrefixLen+1);
            if (js->rpre == NULL)
                s = nats_setDefaultError(NATS_NO_MEMORY);
            else
            {
                char tmp[NATS_INBOX_ARRAY_SIZE];

                natsInbox_init(tmp, sizeof(tmp));
                memcpy(js->rpre, tmp, NATS_INBOX_PRE_LEN);
                memcpy(js->rpre+NATS_INBOX_PRE_LEN, tmp+((int)strlen(tmp)-jsReplyTokenSize), jsReplyTokenSize);
                js->rpre[jsReplyPrefixLen-1] = '.';
                js->rpre[jsReplyPrefixLen]   = '\0';
            }
        }
        if (s == NATS_OK)
        {
            char subj[jsReplyPrefixLen + 2];

            snprintf(subj, sizeof(subj), "%s*", js->rpre);
            s = natsConn_subscribeNoPool(&(js->rsub), js->nc, subj, _handleAsyncReply, (void*) js);
            if (s == NATS_OK)
            {
                _retain(js);
                natsSubscription_SetPendingLimits(js->rsub, -1, -1);
                natsSubscription_SetOnCompleteCB(js->rsub, _subComplete, (void*) js);
            }
        }
        if (s != NATS_OK)
        {
            // Undo the things we created so we retry again next time.
            // It is either that or we have to always check individual
            // objects to know if we have to create them.
            NATS_FREE(js->rpre);
            js->rpre = NULL;
            natsStrHash_Destroy(js->pm);
            js->pm = NULL;
            natsCondition_Destroy(js->cond);
            js->cond = NULL;
        }
    }
    if (s == NATS_OK)
    {
        int64_t l;
        int     i;

        memcpy(reply, js->rpre, jsReplyPrefixLen);
        l = nats_Rand64();
        for (i=0; i < jsReplyTokenSize; i++)
        {
            reply[jsReplyPrefixLen+i] = jsDigits[l%jsBase];
            l /= jsBase;
        }
        reply[jsReplyPrefixLen+jsReplyTokenSize] = '\0';
    }

    return NATS_UPDATE_ERR_STACK(s);
}

static natsStatus
_registerPubMsg(natsConnection **nc, char *reply, jsCtx *js, natsMsg *msg)
{
    natsStatus  s       = NATS_OK;
    char        *id     = NULL;
    bool        release = false;
    int64_t     maxp    = 0;

    js_lock(js);

    maxp = js->opts.PublishAsync.MaxPending;

    js->pmcount++;
    s = _newAsyncReply(reply, js);
    if (s == NATS_OK)
        id = reply+jsReplyPrefixLen;
    if ((s == NATS_OK)
            && (maxp > 0)
            && (js->pmcount > maxp))
    {
        int64_t target = nats_setTargetTime(js->opts.PublishAsync.StallWait);

        _retain(js);

        js->stalled++;
        while ((s != NATS_TIMEOUT) && (js->pmcount > maxp))
            s = natsCondition_AbsoluteTimedWait(js->cond, js->mu, target);
        js->stalled--;

        if (s == NATS_TIMEOUT)
            s = nats_setError(s, "%s", "stalled with too many outstanding async published messages");

        release = true;
    }
    if (s == NATS_OK)
        s = natsStrHash_Set(js->pm, id, true, msg, NULL);
    if (s == NATS_OK)
        *nc = js->nc;
    else
        js->pmcount--;
    if (release)
        js_unlockAndRelease(js);
    else
        js_unlock(js);

    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
js_PublishAsync(jsCtx *js, const char *subj, const void *data, int dataLen,
                jsPubOptions *opts)
{
    natsStatus s;
    natsMsg    *msg = NULL;

    s = natsMsg_Create(&msg, subj, NULL, (const char*) data, dataLen);
    IFOK(s, js_PublishMsgAsync(js, &msg, opts));

    // The `msg` pointer will have been set to NULL if the library took ownership.
    natsMsg_Destroy(msg);

    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
js_PublishMsgAsync(jsCtx *js, natsMsg **msg, jsPubOptions *opts)
{
    natsStatus      s   = NATS_OK;
    natsConnection  *nc = NULL;
    char            reply[jsReplyPrefixLen + jsReplyTokenSize + 1];

    if ((js == NULL) || (msg == NULL) || (*msg == NULL))
        return nats_setDefaultError(NATS_INVALID_ARG);

    if (opts != NULL)
        s = _setHeadersFromOptions(*msg, opts);

    // On success, the context will be retained.
    IFOK(s, _registerPubMsg(&nc, reply, js, *msg));
    if (s == NATS_OK)
    {
        s = natsConn_publish(nc, *msg, (const char*) reply, false);
        if (s != NATS_OK)
        {
            char *id = reply+jsReplyPrefixLen;

            // The message may or may not have been sent, we don't know for sure.
            // We are going to attempt to remove from the map. If we can, then
            // we return the failure and the user owns the message. If we can't
            // it means that its ack has already been processed, so we consider
            // this call a success. If there was a pub ack failure, it is handled
            // with the error callback, but regardless, the library owns the message.
            js_lock(js);
            // If msg no longer in map, Remove() will return NULL.
            if (natsStrHash_Remove(js->pm, id) == NULL)
                s = NATS_OK;
            else
                js->pmcount--;
            js_unlock(js);
        }
    }

    // On success, clear the pointer to the message to indicate that the library
    // now owns it. If user calls natsMsg_Destroy(), it will have no effect since
    // they would call with natsMsg_Destroy(NULL), which is a no-op.
    if (s == NATS_OK)
        *msg = NULL;

    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
js_PublishAsyncComplete(jsCtx *js, jsPubOptions *opts)
{
    natsStatus  s       = NATS_OK;
    int64_t     ttl     = 0;
    int64_t     target  = 0;

    if (js == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);

    if (opts != NULL)
    {
        s = _checkMaxWaitOpt(&ttl, opts);
        if (s != NATS_OK)
            return NATS_UPDATE_ERR_STACK(s);
    }

    js_lock(js);
    if ((js->pm == NULL) || (js->pmcount == 0))
    {
        js_unlock(js);
        return NATS_OK;
    }
    if (ttl > 0)
        target = nats_setTargetTime(ttl);

    _retain(js);
    js->pacw++;
    while ((s != NATS_TIMEOUT) && (js->pmcount > 0))
    {
        if (target > 0)
            s = natsCondition_AbsoluteTimedWait(js->cond, js->mu, target);
        else
            natsCondition_Wait(js->cond, js->mu);
    }
    js->pacw--;

    // Make sure that if we return timeout, there is really
    // still unack'ed publish messages.
    if ((s == NATS_TIMEOUT) && (js->pmcount == 0))
        s = NATS_OK;

    js_unlockAndRelease(js);
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
js_PublishAsyncGetPendingList(natsMsgList *pending, jsCtx *js)
{
    natsStatus          s        = NATS_OK;
    int                 count    = 0;

    if ((pending == NULL) || (js == NULL))
        return nats_setDefaultError(NATS_INVALID_ARG);

    js_lock(js);
    if ((count = natsStrHash_Count(js->pm)) == 0)
    {
        js_unlock(js);
        return NATS_NOT_FOUND;
    }
    pending->Msgs  = (natsMsg**) NATS_CALLOC(count, sizeof(natsMsg*));
    if (pending->Msgs == NULL)
        s = nats_setDefaultError(NATS_NO_MEMORY);
    else
    {
        natsStrHashIter iter;
        void            *val = NULL;
        int             i    = 0;

        natsStrHashIter_Init(&iter, js->pm);
        while (natsStrHashIter_Next(&iter, NULL, &val))
        {
            pending->Msgs[i++] = (natsMsg*) val;
            natsStrHashIter_RemoveCurrent(&iter);
            if (js->pmcount > 0)
                js->pmcount--;
        }
        *(int*)&(pending->Count) = count;
    }
    js_unlock(js);

    if (s != NATS_OK)
        natsMsgList_Destroy(pending);

    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
jsSubOptions_Init(jsSubOptions *opts)
{
    if (opts == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);

    memset(opts, 0, sizeof(jsSubOptions));
    opts->Config.AckPolicy      = -1;
    opts->Config.DeliverPolicy  = -1;
    opts->Config.ReplayPolicy   = -1;
    return NATS_OK;
}

static natsStatus
_lookupStreamBySubject(const char **stream, natsConnection *nc, const char *subject, jsOptions *jo, jsErrCode *errCode)
{
    natsStatus          s       = NATS_OK;
    natsBuffer          *buf    = NULL;
    char                *apiSubj= NULL;
    natsMsg             *resp   = NULL;

    *stream = NULL;

    // Request will be: {"subject":"<subject>"}
    s = natsBuf_Create(&buf, 14 + (int) strlen(subject));
    IFOK(s, natsBuf_Append(buf, "{\"subject\":\"", -1));
    IFOK(s, natsBuf_Append(buf, subject, -1));
    IFOK(s, natsBuf_Append(buf, "\"}", -1));
    if (s == NATS_OK)
    {
        if (nats_asprintf(&apiSubj, jsApiStreams, js_lenWithoutTrailingDot(jo->Prefix), jo->Prefix) < 0)
            s = nats_setDefaultError(NATS_NO_MEMORY);
    }
    // Send the request
    IFOK_JSR(s, natsConnection_Request(&resp, nc, apiSubj, natsBuf_Data(buf), natsBuf_Len(buf), jo->Wait));
    // If no error, decode response
    if ((s == NATS_OK) && (resp != NULL) && (natsMsg_GetDataLength(resp) > 0))
    {
        nats_JSON   *json     = NULL;
        char        **streams = NULL;
        int         count     = 0;
        int         i;

        s = nats_JSONParse(&json, natsMsg_GetData(resp), natsMsg_GetDataLength(resp));
        IFOK(s, nats_JSONGetArrayStr(json, "streams", &streams, &count));

        if ((s == NATS_OK) && (count > 0))
            *stream = streams[0];
        else
            s = nats_setError(NATS_ERR, "%s", jsErrNoStreamMatchesSubject);

        // Do not free the first one since we want to return it.
        for (i=1; i<count; i++)
            NATS_FREE(streams[i]);
        NATS_FREE(streams);
        nats_JSONDestroy(json);
    }

    NATS_FREE(apiSubj);
    natsBuf_Destroy(buf);
    natsMsg_Destroy(resp);

    return NATS_UPDATE_ERR_STACK(s);
}

void
jsSub_free(jsSub *jsi)
{
    jsCtx *js = NULL;

    if (jsi == NULL)
        return;

    js = jsi->js;
    natsTimer_Destroy(jsi->hbTimer);
    NATS_FREE(jsi->stream);
    NATS_FREE(jsi->consumer);
    NATS_FREE(jsi->nxtMsgSubj);
    NATS_FREE(jsi->cmeta);
    NATS_FREE(jsi->fcReply);
    NATS_FREE(jsi);

    js_release(js);
}

static void
_autoAckCB(natsConnection *nc, natsSubscription *sub, natsMsg *msg, void *closure)
{
    jsSub   *jsi = (jsSub*) closure;
    char    _reply[256];
    char    *reply = NULL;
    bool    frply  = false;

    if (msg->reply != NULL)
    {
        if (strlen(msg->reply) < sizeof(_reply))
        {
            snprintf(_reply, sizeof(_reply), "%s", msg->reply);
            reply = _reply;
        }
        else
        {
            reply = NATS_STRDUP(msg->reply);
            frply = (reply != NULL ? true : false);
        }
    }

    // Invoke user callback
    (jsi->usrCb)(nc, sub, msg, jsi->usrCbClosure);

    // Ack the message (unless we got a failure copying the reply subject)
    if (reply == NULL)
        return;

    natsConnection_PublishString(nc, reply, jsAckAck);

    if (frply)
        NATS_FREE(reply);
}

natsStatus
jsSub_deleteConsumer(natsSubscription *sub)
{
    jsCtx       *js       = NULL;
    const char  *stream   = NULL;
    const char  *consumer = NULL;
    natsStatus  s;

    natsSub_Lock(sub);
    if ((sub->jsi != NULL) && (sub->jsi->dc))
    {
        js       = sub->jsi->js;
        stream   = sub->jsi->stream;
        consumer = sub->jsi->consumer;
        // For drain, we could be trying to delete from
        // the thread that checks for drain, or from the
        // user checking from drain completion. So we
        // switch off if we are going to delete now.
        sub->jsi->dc = false;
    }
    natsSub_Unlock(sub);

    if ((js == NULL) || (stream == NULL) || (consumer == NULL))
        return NATS_OK;

    s = js_DeleteConsumer(js, stream, consumer, NULL, NULL);
    if (s == NATS_NOT_FOUND)
        s = nats_setError(s, "failed to delete consumer '%s': not found", consumer);
    return NATS_UPDATE_ERR_STACK(s);
}

// Runs under the subscription lock, but lock will be released,
// the connection lock will be possibly acquired/released, then
// the subscription lock reacquired.
void
jsSub_deleteConsumerAfterDrain(natsSubscription *sub)
{
    natsConnection  *nc       = NULL;
    const char      *consumer = NULL;
    natsStatus      s;

    if ((sub->jsi == NULL) || !sub->jsi->dc)
        return;

    nc       = sub->conn;
    consumer = sub->jsi->consumer;

    // Need to release sub lock since deletion of consumer
    // will require the connection lock, etc..
    natsSub_Unlock(sub);

    s = jsSub_deleteConsumer(sub);
    if (s != NATS_OK)
    {
        natsConn_Lock(nc);
        if (nc->opts->asyncErrCb != NULL)
        {
            char tmp[256];
            snprintf(tmp, sizeof(tmp), "failed to delete consumer '%s': %d (%s)",
                    consumer, s, natsStatus_GetText(s));
            natsAsyncCb_PostErrHandler(nc, sub, s, NATS_STRDUP(tmp));
        }
        natsConn_Unlock(nc);
    }

    // Reacquire the lock before returning.
    natsSub_Lock(sub);
}

static natsStatus
_copyString(char **new_str, const char *str, int l)
{
    *new_str = NATS_MALLOC(l+1);
    if (*new_str == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    memcpy(*new_str, str, l);
    *(*new_str+l) = '\0';
    return NATS_OK;
}

static natsStatus
_getMetaData(const char *reply,
    char **domain,
    char **stream,
    char **consumer,
    uint64_t *numDelivered,
    uint64_t *sseq,
    uint64_t *dseq,
    int64_t *tm,
    uint64_t *numPending,
    int asked)
{
    natsStatus  s    = NATS_OK;
    const char  *p   = reply;
    const char  *np  = NULL;
    const char  *str = NULL;
    int         done = 0;
    int64_t     val  = 0;
    int         nt   = 0;
    int         i, l;
    struct token {
        const char* start;
        int         len;
    };
    struct token tokens[9];

    memset(tokens, 0, sizeof(tokens));

    // v1 version of subject is total of 9 tokens:
    //
    // $JS.ACK.<stream name>.<consumer name>.<num delivered>.<stream sequence>.<consumer sequence>.<timestamp>.<num pending>
    //
    // Since we are here with the 2 first token stripped, the number of tokens is 7.
    //
    // v2 version of subject total tokens is 12:
    //
    // $JS.ACK.<domain>.<account hash>.<stream name>.<consumer name>.<num delivered>.<stream sequence>.<consumer sequence>.<timestamp>.<num pending>.<a token with a random value>
    //
    // Again, since "$JS.ACK." has already been stripped, this is 10 tokens.
    // However, the library does not care about anything after the num pending,
    // so it would be 9 tokens.

    // Find tokens but stop when we have at most 9 tokens.
    while ((nt < 9) && ((np = strchr(p, '.')) != NULL))
    {
        tokens[nt].start = p;
        tokens[nt].len   = (int) (np - p);
        nt++;
        p = (const char*) (np+1);
    }
    if (np == NULL)
    {
        tokens[nt].start = p;
        tokens[nt].len   = (int) (strlen(p));
        nt++;
    }

    // It is invalid if less than 7 or if it has more than 7, it has to have
    // at least 9 to be valid.
    if ((nt < 7) || ((nt > 7) && (nt < 9)))
        return NATS_ERR;

    // If it is 7 tokens (the v1), then insert 2 empty tokens at the beginning.
    if (nt == 7)
    {
        memmove(&(tokens[2]), &(tokens[0]), nt*sizeof(struct token));
        tokens[0].start = NULL;
        tokens[0].len = 0;
        tokens[1].start = NULL;
        tokens[1].len = 0;
        // We work with knowledge that we have now 9 tokens.
        nt = 9;
    }

    for (i=0; i<nt; i++)
    {
        str = tokens[i].start;
        l   = tokens[i].len;
        // For numeric tokens, anything after the consumer name token,
        // which is index 3 (starting at 0).
        if (i > 3)
        {
            val = nats_ParseInt64(str, l);
            // Since we don't expect any negative value,
            // if we get -1, which indicates a parsing error,
            // return this fact.
            if (val == -1)
                return NATS_ERR;
        }
        switch (i)
        {
            case 0:
                if (domain != NULL)
                {
                    // A domain "_" will be sent by new server to indicate
                    // that there is no domain, but to make the number of tokens
                    // constant.
                    if ((str == NULL) || ((l == 1) && (str[0] == '_')))
                        *domain = NULL;
                    else if ((s = _copyString(domain, str, l)) != NATS_OK)
                        return NATS_UPDATE_ERR_STACK(s);
                    done++;
                }
                break;
            case 1:
                // acc hash, ignore.
                break;
            case 2:
                if (stream != NULL)
                {
                    if ((s = _copyString(stream, str, l)) != NATS_OK)
                        return NATS_UPDATE_ERR_STACK(s);
                    done++;
                }
                break;
            case 3:
                if (consumer != NULL)
                {
                    if ((s = _copyString(consumer, str, l)) != NATS_OK)
                        return NATS_UPDATE_ERR_STACK(s);
                    done++;
                }
                break;
            case 4:
                if (numDelivered != NULL)
                {
                    *numDelivered = (uint64_t) val;
                    done++;
                }
                break;
            case 5:
                if (sseq != NULL)
                {
                    *sseq = (uint64_t) val;
                    done++;
                }
                break;
            case 6:
                if (dseq != NULL)
                {
                    *dseq = (uint64_t) val;
                    done++;
                }
                break;
            case 7:
                if (tm != NULL)
                {
                    *tm = val;
                    done++;
                }
                break;
            case 8:
                if (numPending != NULL)
                {
                    *numPending = (uint64_t) val;
                    done++;
                }
                break;
        }
        if (done == asked)
            return NATS_OK;
    }
    return NATS_OK;
}

natsStatus
jsSub_trackSequences(jsSub *jsi, const char *reply)
{
    natsStatus  s = NATS_OK;

    if ((reply == NULL) || (strstr(reply, jsAckPrefix) != reply))
        return NATS_OK;

    // Data is equivalent to HB, so consider active.
    jsi->active = true;

    NATS_FREE(jsi->cmeta);
    DUP_STRING(s, jsi->cmeta, reply+jsAckPrefixLen);
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
jsSub_processSequenceMismatch(natsSubscription *sub, natsMsg *msg, bool *sm)
{
    jsSub       *jsi   = sub->jsi;
    const char  *str   = NULL;
    int64_t     val    = 0;
    natsStatus  s;

    *sm = false;

    // This is an HB, so update that we are active.
    jsi->active = true;

    if (jsi->cmeta == NULL)
        return NATS_OK;

    s = _getMetaData(jsi->cmeta, NULL, NULL, NULL, NULL, &jsi->sseq, &jsi->dseq, NULL, NULL, 2);
    if (s != NATS_OK)
    {
        if (s == NATS_ERR)
            return nats_setError(NATS_ERR, "invalid JS ACK: '%s'", jsi->cmeta);
        return NATS_UPDATE_ERR_STACK(s);
    }

    s = natsMsgHeader_Get(msg, jsLastConsumerSeqHdr, &str);
    if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);

    if (str != NULL)
    {
        // Now that we have the field, we parse it. This function returns
        // -1 if there is a parsing error.
        val = nats_ParseInt64(str, (int) strlen(str));
        if (val == -1)
            return nats_setError(NATS_ERR, "invalid last consumer sequence: '%s'", str);

        jsi->ldseq = (uint64_t) val;
    }
    if (jsi->ldseq == jsi->dseq)
    {
        // Sync subs use this flag to get the NextMsg() to error out and
        // return NATS_MISMATCH to indicate that a mismatch was discovered,
        // but immediately switch it off so that remaining NextMsg() work ok.
        // Here we have resolved the mismatch, so we clear this flag (we
        // could check for sync vs async, but no need to bother).
        jsi->sm = false;
        // Clear the suppression flag.
        jsi->ssmn = false;
    }
    else if (!jsi->ssmn)
    {
        // Record the sequence mismatch.
        jsi->sm = true;
        // Prevent following mismatch report until mismatch is resolved.
        jsi->ssmn = true;
        // Only for async subscriptions, indicate that the connection should
        // push a NATS_MISMATCH to the async callback.
        if (sub->msgCb != NULL)
            *sm = true;
    }
    return NATS_OK;
}

natsStatus
natsSubscription_GetSequenceMismatch(jsConsumerSequenceMismatch *csm, natsSubscription *sub)
{
    jsSub *jsi;

    if ((csm == NULL) || (sub == NULL))
        return nats_setDefaultError(NATS_INVALID_ARG);

    natsSubAndLdw_Lock(sub);
    if (sub->jsi == NULL)
    {
        natsSubAndLdw_Unlock(sub);
        return nats_setError(NATS_INVALID_SUBSCRIPTION, "%s", jsErrNotAJetStreamSubscription);
    }
    jsi = sub->jsi;
    if (jsi->dseq == jsi->ldseq)
    {
        natsSubAndLdw_Unlock(sub);
        return NATS_NOT_FOUND;
    }
    memset(csm, 0, sizeof(jsConsumerSequenceMismatch));
    csm->Stream = jsi->sseq;
    csm->ConsumerClient = jsi->dseq;
    csm->ConsumerServer = jsi->ldseq;
    natsSubAndLdw_Unlock(sub);
    return NATS_OK;
}

natsStatus
jsSub_scheduleFlowControlResponse(jsSub *jsi, natsSubscription *sub, const char *reply)
{
    NATS_FREE(jsi->fcReply);
    jsi->fcReply = NATS_STRDUP(reply);
    if (jsi->fcReply == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    jsi->fcDelivered = sub->delivered + (uint64_t) sub->msgList.msgs;

    return NATS_OK;
}

static natsStatus
_checkMsg(natsMsg *msg, bool checkSts, bool *usrMsg, jsErrCode *jerr)
{
    natsStatus  s    = NATS_OK;
    const char  *val = NULL;
    const char  *desc= NULL;

    *usrMsg = true;
    if (jerr != NULL)
        *jerr = 0;

    if ((msg->dataLen > 0) || (msg->hdrLen <= 0))
        return NATS_OK;

    s = natsMsgHeader_Get(msg, STATUS_HDR, &val);
    // If no status header, this is still considered a user message, so OK.
    if (s == NATS_NOT_FOUND)
        return NATS_OK;
    // If serious error, return it.
    else if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);

    // At this point, this is known to be a status message, not a user message.
    *usrMsg = false;

    // If we don't care about status, we are done.
    if (!checkSts)
        return NATS_OK;

    // 404 indicating that there are no messages.
    if (strncmp(val, NOT_FOUND_STATUS, HDR_STATUS_LEN) == 0)
        return NATS_NOT_FOUND;

    // Older servers may send a 408 when a request in the server was expired
    // and interest is still found, which will be the case for our
    // implementation. Regardless, ignore 408 errors, the caller will
    // go back to wait for the next message.
    if (strncmp(val, REQ_TIMEOUT, HDR_STATUS_LEN) == 0)
        return NATS_OK;

    // The possible 503 is handled directly in natsSub_nextMsg(), so we
    // would never get it here in this function.

    natsMsgHeader_Get(msg, DESCRIPTION_HDR, &desc);
    return nats_setError(NATS_ERR, "%s", (desc == NULL ? "error checking pull subscribe message" : desc));
}

static natsStatus
_sendPullRequest(natsConnection *nc, const char *subj, const char *rply,
                 natsBuffer *buf, int batchSize, int64_t *timeout, int64_t start, bool noWait)
{
    natsStatus  s;
    int64_t     expires;

    *timeout -= (nats_Now()-start);
    if (*timeout <= 0)
    {
        // At this point, consider that we have timed-out.
        return nats_setDefaultError(NATS_TIMEOUT);
    }

    // Make our request expiration a bit shorter than the
    // current timeout.
    expires = (*timeout >= 20 ? *timeout - 10 : *timeout);

    // Since "expires" is a Go time.Duration and our timeout
    // is in milliseconds, convert it to nanos.
    expires *= 1000000;

    natsBuf_Reset(buf);
    s = natsBuf_AppendByte(buf, '{');
    IFOK(s, nats_marshalLong(buf, false, "batch", (int64_t) batchSize));
    IFOK(s, nats_marshalLong(buf, true, "expires", expires));
    if ((s == NATS_OK) && noWait)
        s = natsBuf_Append(buf, ",\"no_wait\":true", -1);
    IFOK(s, natsBuf_AppendByte(buf, '}'));

    // Sent the request to get more messages.
    IFOK(s, natsConnection_PublishRequest(nc, subj, rply,
        natsBuf_Data(buf), natsBuf_Len(buf)));

    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
natsSubscription_Fetch(natsMsgList *list, natsSubscription *sub, int batch, int64_t timeout,
                       jsErrCode *errCode)
{
    natsStatus      s       = NATS_OK;
    natsMsg         **msgs  = NULL;
    int             count   = 0;
    natsConnection  *nc     = NULL;
    const char      *subj   = NULL;
    const char      *rply   = NULL;
    int             pmc     = 0;
    char            buffer[64];
    natsBuffer      buf;
    int64_t         start;

    if (errCode != NULL)
        *errCode = 0;

    if (list == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);

    memset(list, 0, sizeof(natsMsgList));

    if ((sub == NULL) || (batch <= 0))
        return nats_setDefaultError(NATS_INVALID_ARG);

    if (timeout <= 0)
        return nats_setDefaultError(NATS_INVALID_TIMEOUT);

    natsSub_Lock(sub);
    if ((sub->jsi == NULL) || !sub->jsi->pull)
    {
        natsSub_Unlock(sub);
        return nats_setError(NATS_INVALID_SUBSCRIPTION, "%s", jsErrNotAPullSubscription);
    }
    msgs = (natsMsg**) NATS_CALLOC(batch, sizeof(natsMsg*));
    if (msgs == NULL)
    {
        natsSub_Unlock(sub);
        return nats_setDefaultError(NATS_NO_MEMORY);
    }
    natsBuf_InitWithBackend(&buf, buffer, 0, sizeof(buffer));
    nc   = sub->conn;
    rply = (const char*) sub->subject;
    subj = sub->jsi->nxtMsgSubj;
    pmc  = (sub->msgList.msgs > 0);
    natsSub_Unlock(sub);

    start = nats_Now();

    // First, if there are already pending messages in the internal sub,
    // then get as much messages as we can (but not more than the batch).
    while (pmc && (s == NATS_OK) && (count < batch))
    {
        natsMsg *msg  = NULL;
        bool    usrMsg= false;

        // This call will pull messages from the internal sync subscription
        // but will not wait (and return NATS_TIMEOUT without updating
        // the error stack) if there are no messages.
        s = natsSub_nextMsg(&msg, sub, 0, true);
        if (s == NATS_OK)
        {
            // Here we care only about user messages.
            s = _checkMsg(msg, false, &usrMsg, errCode);
            if ((s == NATS_OK) && usrMsg)
                msgs[count++] = msg;
            else
                natsMsg_Destroy(msg);
        }
    }

    // If we have OK or TIMEOUT and not all messages, we will send a fetch
    // request to the server.
    if (((s == NATS_OK) || (s == NATS_TIMEOUT)) && (count != batch))
    {
        bool doNoWait = (batch-count > 1 ? true : false);

        s = _sendPullRequest(nc, subj, rply, &buf, batch-count,
                             &timeout, start, doNoWait);
        // Now wait for messages or a 404 saying that there are no more.
        while ((s == NATS_OK) && (count < batch))
        {
            natsMsg *msg    = NULL;
            bool    usrMsg  = false;

            s = natsSub_nextMsg(&msg, sub, timeout, true);
            if (s == NATS_OK)
            {
                s = _checkMsg(msg, true, &usrMsg, errCode);
                if ((s == NATS_OK) && usrMsg)
                    msgs[count++] = msg;
                else
                {
                    natsMsg_Destroy(msg);
                    // If we have a 404 for our "no_wait" request and have
                    // not collected any message, then resend request to
                    // wait this time.
                    if (doNoWait && (s == NATS_NOT_FOUND) && (count == 0))
                    {
                        doNoWait = false;

                        s = _sendPullRequest(nc, subj, rply, &buf, batch-count,
                                             &timeout, start, false);
                    }
                }
            }
        }
    }

    natsBuf_Destroy(&buf);

    // If count > 0 it means that we have gathered some user messages,
    // so we need to return them to the user with a NATS_OK status.
    if (count > 0)
    {
        // If there was an error, we need to clear the error stack,
        // since we return NATS_OK.
        if (s != NATS_OK)
            nats_clearLastError();

        // Update the list with what we have collected.
        list->Msgs = msgs;
        *(int*)&(list->Count) = count;

        return NATS_OK;
    }

    NATS_FREE(msgs);

    return NATS_UPDATE_ERR_STACK(s);
}

static void
_hbTimerFired(natsTimer *timer, void* closure)
{
    natsSubscription    *sub = (natsSubscription*) closure;
    jsSub               *jsi = sub->jsi;
    bool                alert= false;
    natsConnection      *nc  = NULL;

    natsSubAndLdw_Lock(sub);
    alert = !jsi->active;
    jsi->active = false;
    nc = sub->conn;
    natsSubAndLdw_Unlock(sub);

    if (!alert)
        return;

    natsConn_Lock(nc);
    // We did create the timer only knowing that there was a async err
    // handler, but check anyway in case we decide to have timer set
    // regardless.
    if (nc->opts->asyncErrCb != NULL)
        natsAsyncCb_PostErrHandler(nc, sub, NATS_MISSED_HEARTBEAT, NULL);
    natsConn_Unlock(nc);
}

// This is invoked when the subscription is destroyed, since in NATS C
// client, timers will automatically fire again, so this callback is
// invoked when the timer has been stopped (and we are ready to destroy it).
static void
_hbTimerStopped(natsTimer *timer, void* closure)
{
    natsSubscription *sub = (natsSubscription*) closure;

    natsSub_release(sub);
}

static bool
_stringPropertyDiffer(const char *user, const char *server)
{
    if (nats_IsStringEmpty(user))
        return false;

    if (nats_IsStringEmpty(server))
        return true;

    return (strcmp(user, server) != 0 ? true : false);
}

#define CFG_CHECK_ERR_START "configuration requests %s to be "
#define CFG_CHECK_ERR_END   ", but consumer's value is "

static natsStatus
_checkConfig(jsConsumerConfig *s, jsConsumerConfig *u)
{
    if (_stringPropertyDiffer(u->Durable, s->Durable))
        return nats_setError(NATS_ERR, CFG_CHECK_ERR_START "'%s'" CFG_CHECK_ERR_END "'%s'", "durable", u->Durable, s->Durable);

    if (_stringPropertyDiffer(u->Description, s->Description))
        return nats_setError(NATS_ERR, CFG_CHECK_ERR_START "'%s'" CFG_CHECK_ERR_END "'%s'", "description", u->Description, s->Description);

    if ((int) u->DeliverPolicy >= 0 && u->DeliverPolicy != s->DeliverPolicy)
        return nats_setError(NATS_ERR, CFG_CHECK_ERR_START "%d" CFG_CHECK_ERR_END "%d", "deliver policy", u->DeliverPolicy, s->DeliverPolicy);

    if (u->OptStartSeq > 0 && u->OptStartSeq != s->OptStartSeq)
        return nats_setError(NATS_ERR, CFG_CHECK_ERR_START "%" PRIu64 CFG_CHECK_ERR_END "%" PRIu64, "optional start sequence", u->OptStartSeq, s->OptStartSeq);

    if (u->OptStartTime > 0 && u->OptStartTime != s->OptStartTime)
        return nats_setError(NATS_ERR, CFG_CHECK_ERR_START "%" PRId64 CFG_CHECK_ERR_END "%" PRId64, "optional start time", u->OptStartTime, s->OptStartTime);

    if ((int) u->AckPolicy >= 0 && u->AckPolicy != s->AckPolicy)
        return nats_setError(NATS_ERR, CFG_CHECK_ERR_START "%d" CFG_CHECK_ERR_END "%d", "ack policy", u->AckPolicy, s->AckPolicy);

    if (u->AckWait > 0 && u->AckWait != s->AckWait)
        return nats_setError(NATS_ERR, CFG_CHECK_ERR_START "%" PRId64 CFG_CHECK_ERR_END "%" PRId64, "ack wait", u->AckWait, s->AckWait);

    if (u->MaxDeliver > 0 && u->MaxDeliver != s->MaxDeliver)
        return nats_setError(NATS_ERR, CFG_CHECK_ERR_START "%" PRId64 CFG_CHECK_ERR_END "%" PRId64, "max deliver", u->MaxDeliver, s->MaxDeliver);

    if ((int) u->ReplayPolicy >= 0 && u->ReplayPolicy != s->ReplayPolicy)
        return nats_setError(NATS_ERR, CFG_CHECK_ERR_START "%d" CFG_CHECK_ERR_END "%d", "replay policy", u->ReplayPolicy, s->ReplayPolicy);

    if (u->RateLimit > 0 && u->RateLimit != s->RateLimit)
        return nats_setError(NATS_ERR, CFG_CHECK_ERR_START "%" PRIu64 CFG_CHECK_ERR_END "%" PRIu64, "rate limit", u->RateLimit, s->RateLimit);

    if (_stringPropertyDiffer(u->SampleFrequency, s->SampleFrequency))
        return nats_setError(NATS_ERR, CFG_CHECK_ERR_START "'%s'" CFG_CHECK_ERR_END "'%s'", "sample frequency", u->SampleFrequency, s->SampleFrequency);

    if (u->MaxWaiting > 0 && u->MaxWaiting != s->MaxWaiting)
        return nats_setError(NATS_ERR, CFG_CHECK_ERR_START "%" PRId64 CFG_CHECK_ERR_END "%" PRId64, "max waiting", u->MaxWaiting, s->MaxWaiting);

    if (u->MaxAckPending > 0 && u->MaxAckPending != s->MaxAckPending)
    {
        return nats_setError(NATS_ERR, CFG_CHECK_ERR_START "%" PRId64 CFG_CHECK_ERR_END "%" PRId64, "max ack pending", u->MaxAckPending, s->MaxAckPending);
    }

	// For flow control, we want to fail if the user explicit wanted it, but
	// it is not set in the existing consumer. If it is not asked by the user,
	// the library still handles it and so no reason to fail.
	if (u->FlowControl && !s->FlowControl)
        return nats_setError(NATS_ERR, CFG_CHECK_ERR_START "'%s'" CFG_CHECK_ERR_END "'%s'", "flow control", "true", "false");

    if (u->Heartbeat > 0 && u->Heartbeat != s->Heartbeat)
        return nats_setError(NATS_ERR, CFG_CHECK_ERR_START "%" PRId64 CFG_CHECK_ERR_END "%" PRId64, "heartbeat", u->Heartbeat, s->Heartbeat);

    return NATS_OK;
}

static natsStatus
_processConsInfo(const char **dlvSubject, jsConsumerInfo *info, jsConsumerConfig *userCfg,
                 bool isPullMode, const char *subj, const char *queue)
{
    bool                dlvSubjEmpty = false;
    jsConsumerConfig    *ccfg        = info->Config;
    const char          *dg          = NULL;
    natsStatus          s            = NATS_OK;

    *dlvSubject = NULL;

	// Make sure this new subject matches or is a subset.
	if (!nats_IsStringEmpty(ccfg->FilterSubject) && (strcmp(subj, ccfg->FilterSubject) != 0))
        return nats_setError(NATS_ERR, "subject '%s' does not match consumer filter subject '%s'",
                             subj, ccfg->FilterSubject);

    // Check that if user wants to create a queue sub,
    // the consumer has no HB nor FC.
    queue = (nats_IsStringEmpty(queue) ? NULL : queue);

    if (queue != NULL)
    {
        if (ccfg->Heartbeat > 0)
            return nats_setError(NATS_ERR, "%s", jsErrNoHeartbeatForQueueSub);

        if (ccfg->FlowControl)
            return nats_setError(NATS_ERR, "%s", jsErrNoFlowControlForQueueSub);
    }

    dlvSubjEmpty = nats_IsStringEmpty(ccfg->DeliverSubject);

	// Prevent binding a subscription against incompatible consumer types.
	if (isPullMode && !dlvSubjEmpty)
    {
        return nats_setError(NATS_ERR, "%s", jsErrPullSubscribeToPushConsumer);
    }
    else if (!isPullMode && dlvSubjEmpty)
    {
        return nats_setError(NATS_ERR, "%s", jsErrPullSubscribeRequired);
    }

	// If pull mode, nothing else to check here.
	if (isPullMode)
    {
        s = _checkConfig(ccfg, userCfg);
        return NATS_UPDATE_ERR_STACK(s);
    }

	// At this point, we know the user wants push mode, and the JS consumer is
	// really push mode.
    dg = ccfg->DeliverGroup;

	if (nats_IsStringEmpty(dg))
    {
		// Prevent an user from attempting to create a queue subscription on
		// a JS consumer that was not created with a deliver group.
		if (queue != NULL)
        {
			return nats_setError(NATS_ERR, "%s",
                                 "cannot create a queue subscription for a consumer without a deliver group");
		}
        else if (info->PushBound)
        {
			// Need to reject a non queue subscription to a non queue consumer
			// if the consumer is already bound.
			return nats_setError(NATS_ERR, "%s", "consumer is already bound to a subscription");
		}
	}
    else
    {
		// If the JS consumer has a deliver group, we need to fail a non queue
		// subscription attempt:
		if (queue == NULL)
        {
			return nats_setError(NATS_ERR,
                                "cannot create a subscription for a consumer with a deliver group %s",
                                dg);
		}
        else if (strcmp(queue, dg) != 0)
        {
			// Here the user's queue group name does not match the one associated
			// with the JS consumer.
			return nats_setError(NATS_ERR,
                                 "cannot create a queue subscription '%s' for a consumer with a deliver group '%s'",
				                 queue, dg);
		}
	}
    s = _checkConfig(ccfg, userCfg);
    if (s == NATS_OK)
        *dlvSubject = ccfg->DeliverSubject;

    return NATS_UPDATE_ERR_STACK(s);
}


static natsStatus
_subscribe(natsSubscription **new_sub, jsCtx *js, const char *subject, const char *pullDurable,
           natsMsgHandler usrCB, void *usrCBClosure, bool isPullMode,
           jsOptions *jsOpts, jsSubOptions *opts, jsErrCode *errCode)
{
    natsStatus          s           = NATS_OK;
    const char          *stream     = NULL;
    const char          *consumer   = NULL;
    const char          *durable    = NULL;
    const char          *deliver    = NULL;
    jsErrCode           jerr        = 0;
    jsConsumerInfo      *info       = NULL;
    bool                lookupErr   = false;
    bool                consBound   = false;
    bool                isQueue     = false;
    natsConnection      *nc         = NULL;
    bool                freePfx     = false;
    bool                freeStream  = false;
    jsSub               *jsi        = NULL;
    int64_t             hbi         = 0;
    bool                create      = false;
    natsSubscription    *sub        = NULL;
    natsMsgHandler      cb          = NULL;
    void                *cbClosure  = NULL;
    char                inbox[NATS_INBOX_ARRAY_SIZE];
    jsOptions           jo;
    jsSubOptions        o;
    jsConsumerConfig    cfgStack;
    jsConsumerConfig    *cfg = NULL;

    if ((new_sub == NULL) || (js == NULL) || nats_IsStringEmpty(subject))
        return nats_setDefaultError(NATS_INVALID_ARG);

    s = js_setOpts(&nc, &freePfx, js, jsOpts, &jo);
    if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);

    // If `opts` is not specified, point to a stack initialized one so
    // we don't have to keep checking if `opts` is NULL or not.
    if (opts == NULL)
    {
        jsSubOptions_Init(&o);
        opts = &o;
    }
    // If user configures optional start sequence or time, the deliver policy
    // need to be updated accordingly. Server will return error if user tries to have both set.
    if (opts->Config.OptStartSeq > 0)
        opts->Config.DeliverPolicy = js_DeliverByStartSequence;
    if (opts->Config.OptStartTime > 0)
        opts->Config.DeliverPolicy = js_DeliverByStartTime;

    isQueue  = !nats_IsStringEmpty(opts->Queue);
    stream   = opts->Stream;
    durable  = (pullDurable != NULL ? pullDurable : opts->Config.Durable);
    consumer = opts->Consumer;
    consBound= (!nats_IsStringEmpty(stream) && !nats_IsStringEmpty(consumer));

    if (isQueue)
    {
        // Reject a user configuration that would want to define hearbeats with
        // a queue subscription.
        if (opts->Config.Heartbeat > 0)
            return nats_setError(NATS_INVALID_ARG, "%s", jsErrNoHeartbeatForQueueSub);
        // Same for flow control
        if (opts->Config.FlowControl)
            return nats_setError(NATS_INVALID_ARG, "%s", jsErrNoFlowControlForQueueSub);
    }

    // In case a consumer has not been set explicitly, then the durable name
    // will be used as the consumer name (after that, `consumer` will still be
    // possibly NULL).
    if (nats_IsStringEmpty(consumer))
    {
        // If this is a queue sub and no durable name was provided, use
        // the queue name as the durable.
        if (isQueue && nats_IsStringEmpty(durable))
            durable = opts->Queue;

        consumer = durable;
    }

    // Find the stream mapped to the subject if not bound to a stream already,
    // that is, if user did not provide a `Stream` name through options).
    if (nats_IsStringEmpty(stream))
    {
        s = _lookupStreamBySubject(&stream, nc, subject, &jo, errCode);
        if (s != NATS_OK)
            goto END;

        freeStream = true;
    }

    // If a consumer name is specified, try to lookup the consumer and
    // if it exists, will attach to it.
    if (!nats_IsStringEmpty(consumer))
    {
        s = js_GetConsumerInfo(&info, js, stream, consumer, &jo, &jerr);
        lookupErr = (s == NATS_TIMEOUT) || (jerr == JSNotEnabledErr);
    }

PROCESS_INFO:
    if (info != NULL)
    {
        if (info->Config == NULL)
        {
            s = nats_setError(NATS_ERR, "%s", "no configuration in consumer info");
            goto END;
        }
        s = _processConsInfo(&deliver, info, &(opts->Config), isPullMode, subject, opts->Queue);
        if (s != NATS_OK)
            goto END;

        // Capture the HB interval (convert in millsecond since Go duration is in nanos)
        hbi = info->Config->Heartbeat / 1000000;
    }
    else if (((s != NATS_OK) && (s != NATS_NOT_FOUND)) || ((s == NATS_NOT_FOUND) && consBound))
    {
        // If the consumer is being bound and got an error on pull subscribe then allow the error.
        if (!(isPullMode && lookupErr && consBound))
            goto END;

        s = NATS_OK;
    }
    else
    {
        s = NATS_OK;
        // Make a shallow copy of the provided consumer config
        // since we may have to change some fields before calling
        // AddConsumer.
        cfg = &cfgStack;
        memcpy(cfg, &(opts->Config), sizeof(jsConsumerConfig));

        if (!isPullMode)
        {
            // Attempt to create consumer if not found nor binding.
            natsInbox_init(inbox, sizeof(inbox));
            deliver = (const char*) inbox;
            cfg->DeliverSubject = deliver;
        }

        // Set config durable with "durable" variable, which will
        // possibly be NULL.
        cfg->Durable = durable;

        // Set DeliverGroup to queue name, possibly NULL
        cfg->DeliverGroup = opts->Queue;

        // Do filtering always, server will clear as needed.
        cfg->FilterSubject = subject;

        // If we have acks at all and the MaxAckPending is not set go ahead
        // and set to the internal max.
        if ((cfg->MaxAckPending == 0) && (cfg->AckPolicy != js_AckNone))
            cfg->MaxAckPending = NATS_OPTS_DEFAULT_MAX_PENDING_MSGS;

        // Capture the HB interval (convert in millsecond since Go duration is in nanos)
        hbi = cfg->Heartbeat / 1000000;

        create = true;
    }
    if (s == NATS_OK)
    {
        jsi = (jsSub*) NATS_CALLOC(1, sizeof(jsSub));
        if (jsi == NULL)
            s = nats_setDefaultError(NATS_NO_MEMORY);
        else
        {
            if (isPullMode)
            {
                if (nats_asprintf(&(jsi->nxtMsgSubj), jsApiRequestNextT, jo.Prefix, stream, consumer) < 0)
                    s = nats_setDefaultError(NATS_NO_MEMORY);
            }
            IF_OK_DUP_STRING(s, jsi->stream, stream);
            if (s == NATS_OK)
            {
                jsi->js     = js;
                jsi->hbi    = hbi;
                jsi->pull   = isPullMode;
                js_retain(js);

                if ((usrCB != NULL) && !opts->ManualAck && (opts->Config.AckPolicy != js_AckNone))
                {
                    // Keep track of user provided CB and closure
                    jsi->usrCb          = usrCB;
                    jsi->usrCbClosure   = usrCBClosure;
                    // Use our own when creating the NATS subscription.
                    cb          = _autoAckCB;
                    cbClosure   = (void*) jsi;
                }
                else if (usrCB != NULL)
                {
                    cb        = usrCB;
                    cbClosure = usrCBClosure;
                }
            }
        }
    }
    if (s == NATS_OK)
    {
        if (isPullMode)
        {
            s = natsInbox_init(inbox, sizeof(inbox));
            deliver = (const char*) inbox;
        }
        // Create the NATS subscription on given deliver subject. Note that
        // cb/cbClosure will be NULL for sync or pull subscriptions.
        IFOK(s, natsConn_subscribeImpl(&sub, nc, true, deliver,
                                       opts->Queue, 0, cb, cbClosure, false, jsi));
        if ((s == NATS_OK) && (hbi > 0))
        {
            bool ct = false; // create timer or not.

            // Check to see if it is even worth creating a timer to check
            // on missed heartbeats, since the way to notify the user will be
            // through async callback.
            natsConn_Lock(nc);
            ct = (nc->opts->asyncErrCb != NULL ? true : false);
            natsConn_Unlock(nc);

            if (ct)
            {
                natsSub_Lock(sub);
                sub->refs++;
                s = natsTimer_Create(&jsi->hbTimer, _hbTimerFired, _hbTimerStopped, hbi*2, (void*) sub);
                if (s != NATS_OK)
                    sub->refs--;
                natsSub_Unlock(sub);
            }
        }
    }
    if ((s == NATS_OK) && create)
    {
        // Multiple subscribers could compete in creating the first consumer
        // that will be shared using the same durable name. If this happens, then
        // do a lookup of the consumer info subscribe using the latest info.
        s = js_AddConsumer(&info, js, stream, cfg, &jo, &jerr);
        if (s != NATS_OK)
        {
            if ((jerr != JSConsumerExistingActiveErr) && (jerr != JSConsumerNameExistErr))
                goto END;

            jsConsumerInfo_Destroy(info);
            info = NULL;

            s = js_GetConsumerInfo(&info, js, stream, consumer, &jo, &jerr);
            if (s != NATS_OK)
                goto END;

            // We will re-create the sub/jsi, so destroy here and go back to point where
            // we process the consumer info response.
            natsSubscription_Destroy(sub);
            sub = NULL;
            jsi = NULL;
            create = false;

            goto PROCESS_INFO;
        }
        else
        {
            natsSub_Lock(sub);
            jsi->dc = true;
            DUP_STRING(s, jsi->consumer, info->Name);
            natsSub_Unlock(sub);
        }
    }

END:
    if (s == NATS_OK)
    {
        *new_sub = sub;
    }
    else
    {
        if (sub == NULL)
            jsSub_free(jsi);
        else
            natsSubscription_Destroy(sub);

        if (errCode != NULL)
            *errCode = jerr;
    }

    // Common cleanup regardless of success or not.
    jsConsumerInfo_Destroy(info);
    if (freePfx)
        NATS_FREE((char*) jo.Prefix);
    if (freeStream)
        NATS_FREE((char*) stream);

    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
js_Subscribe(natsSubscription **sub, jsCtx *js, const char *subject,
             natsMsgHandler cb, void *cbClosure,
             jsOptions *jsOpts, jsSubOptions *opts, jsErrCode *errCode)
{
    natsStatus s;

    if (errCode != NULL)
        *errCode = 0;

    if (cb == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);

    s = _subscribe(sub, js, subject, NULL, cb, cbClosure, false, jsOpts, opts, errCode);
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
js_SubscribeSync(natsSubscription **sub, jsCtx *js, const char *subject,
                 jsOptions *jsOpts, jsSubOptions *opts, jsErrCode *errCode)
{
    natsStatus s;

    if (errCode != NULL)
        *errCode = 0;

    s = _subscribe(sub, js, subject, NULL, NULL, NULL, false, jsOpts, opts, errCode);
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
js_PullSubscribe(natsSubscription **sub, jsCtx *js, const char *subject, const char *durable,
                 jsOptions *jsOpts, jsSubOptions *opts, jsErrCode *errCode)
{
    natsStatus s;

    if (errCode != NULL)
        *errCode = 0;

    if (nats_IsStringEmpty(durable))
        return nats_setError(NATS_INVALID_ARG, "%s", jsErrDurRequired);

    // Check for invalid ack policy
    if (opts != NULL)
    {
        jsAckPolicy p = (opts->Config.AckPolicy);

        if ((p == js_AckNone) || (p == js_AckAll))
        {
            const char *ap = (p == js_AckNone ? jsAckNoneStr : jsAckAllStr);
            return nats_setError(NATS_INVALID_ARG,
                                 "invalid ack mode '%s' for pull consumers", ap);
        }
    }

    s = _subscribe(sub, js, subject, durable, NULL, NULL, true, jsOpts, opts, errCode);
    return NATS_UPDATE_ERR_STACK(s);
}

static natsStatus
_ackMsg(natsMsg *msg, jsOptions *opts, const char *ackType, bool inProgress, bool sync, jsErrCode *errCode)
{
    natsSubscription    *sub = NULL;
    natsConnection      *nc  = NULL;
    jsCtx               *js  = NULL;
    jsSub               *jsi = NULL;
    natsStatus          s    = NATS_OK;

    if (msg == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);

    if (natsMsg_isAcked(msg))
        return NATS_OK;

    if (msg->sub == NULL)
        return nats_setError(NATS_ILLEGAL_STATE, "%s", jsErrMsgNotBound);

    if (nats_IsStringEmpty(msg->reply))
        return nats_setError(NATS_ILLEGAL_STATE, "%s", jsErrMsgNotJS);

    // All these are immutable and don't need locking.
    sub = msg->sub;
    jsi = sub->jsi;
    js = jsi->js;
    nc = sub->conn;

    if (sync)
    {
        natsMsg *rply   = NULL;
        int64_t wait    = (opts != NULL ? opts->Wait : 0);

        if (wait == 0)
        {
            // When getting a context, if user did not specify a wait,
            // we default to jsDefaultRequestWait, so this won't be 0.
            js_lock(js);
            wait = js->opts.Wait;
            js_unlock(js);
        }
        IFOK_JSR(s, natsConnection_RequestString(&rply, nc, msg->reply, ackType, wait));
        natsMsg_Destroy(rply);
    }
    else
    {
        s = natsConnection_PublishString(nc, msg->reply, ackType);
    }
    // Indicate that we have ack'ed the message
    if ((s == NATS_OK) && !inProgress)
        natsMsg_setAcked(msg);

    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
natsMsg_Ack(natsMsg *msg, jsOptions *opts)
{
    return _ackMsg(msg, opts, jsAckAck, false, false, NULL);
}

natsStatus
natsMsg_AckSync(natsMsg *msg, jsOptions *opts, jsErrCode *errCode)
{
    return _ackMsg(msg, opts, jsAckAck, false, true, errCode);
}

natsStatus
natsMsg_Nak(natsMsg *msg, jsOptions *opts)
{
    return _ackMsg(msg, opts, jsAckNak, false, false, NULL);
}

natsStatus
natsMsg_InProgress(natsMsg *msg, jsOptions *opts)
{
    return _ackMsg(msg, opts, jsAckInProgress, true, false, NULL);
}

natsStatus
natsMsg_Term(natsMsg *msg, jsOptions *opts)
{
    return _ackMsg(msg, opts, jsAckTerm, false, false, NULL);
}

natsStatus
natsMsg_GetMetaData(jsMsgMetaData **new_meta, natsMsg *msg)
{
    jsMsgMetaData   *meta = NULL;
    natsStatus      s;

    if ((new_meta == NULL) || (msg == NULL))
        return nats_setDefaultError(NATS_INVALID_ARG);

     if (msg->sub == NULL)
        return nats_setError(NATS_ILLEGAL_STATE, "%s", jsErrMsgNotBound);

    if (nats_IsStringEmpty(msg->reply))
        return nats_setError(NATS_ILLEGAL_STATE, "%s", jsErrMsgNotJS);

    if (strstr(msg->reply, jsAckPrefix) != msg->reply)
        return nats_setError(NATS_ERR, "invalid meta data '%s'", msg->reply);

    meta = (jsMsgMetaData*) NATS_CALLOC(1, sizeof(jsMsgMetaData));
    if (meta == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    s = _getMetaData(msg->reply+jsAckPrefixLen,
                        &(meta->Domain),
                        &(meta->Stream),
                        &(meta->Consumer),
                        &(meta->NumDelivered),
                        &(meta->Sequence.Stream),
                        &(meta->Sequence.Consumer),
                        &(meta->Timestamp),
                        &(meta->NumPending),
                        8);
    if (s == NATS_ERR)
        s = nats_setError(NATS_ERR, "invalid meta data '%s'", msg->reply);

    if (s == NATS_OK)
        *new_meta = meta;
    else
        jsMsgMetaData_Destroy(meta);

    return NATS_UPDATE_ERR_STACK(s);
}

void
jsMsgMetaData_Destroy(jsMsgMetaData *meta)
{
    if (meta == NULL)
        return;

    NATS_FREE(meta->Stream);
    NATS_FREE(meta->Consumer);
    NATS_FREE(meta->Domain);
    NATS_FREE(meta);
}

bool
natsMsg_isJSCtrl(natsMsg *msg, int *ctrlType)
{
    char *p = NULL;

    *ctrlType = 0;

    if ((msg->dataLen > 0) || (msg->hdrLen <= 0))
        return false;

    if (strstr(msg->hdr, HDR_LINE_PRE) != msg->hdr)
        return false;

    p = msg->hdr + HDR_LINE_PRE_LEN;
    if (*p != ' ')
        return false;

    while ((*p != '\0') && isspace(*p))
        p++;

    if ((*p == '\r') || (*p == '\n') || (*p == '\0'))
        return false;

    if (strstr(p, CTRL_STATUS) != p)
        return false;

    p += HDR_STATUS_LEN;

    if (!isspace(*p))
        return false;

    while (isspace(*p))
        p++;

    if (strstr(p, "Idle") == p)
        *ctrlType = jsCtrlHeartbeat;
    else if (strstr(p, "Flow") == p)
        *ctrlType = jsCtrlFlowControl;

    return true;
}
