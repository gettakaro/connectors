// Thin wrapper around DayZ's RestApi/RestContext.
// Enforce Script's REST API is callback-based; we wrap each call with a
// dedicated RestCallback so we can deliver the response to whoever asked.

class TakaroHttpResponse
{
    int statusCode;
    string body;
    bool ok;
    string errorMessage;
}

// Function pointer style: subclasses of TakaroHttpCallback hold the lambda.
class TakaroHttpCallback : RestCallback
{
    string m_Tag;
    int m_StatusCode;
    string m_ResponseBody;
    bool m_Done;
    bool m_Ok;

    void TakaroHttpCallback(string tag = "")
    {
        m_Tag = tag;
        m_Done = false;
        m_Ok = false;
        m_StatusCode = 0;
        m_ResponseBody = "";
    }

    override void OnSuccess(string data, int dataSize)
    {
        m_Ok = true;
        m_StatusCode = 200;
        m_ResponseBody = data;
        m_Done = true;
        TakaroLog.Debug("HTTP " + m_Tag + " OK (" + dataSize.ToString() + " bytes)");
        OnComplete();
    }

    override void OnError(int errorCode)
    {
        m_Ok = false;
        m_StatusCode = errorCode;
        m_ResponseBody = "";
        m_Done = true;
        TakaroLog.Debug("HTTP " + m_Tag + " error code " + errorCode.ToString());
        OnComplete();
    }

    override void OnTimeout()
    {
        m_Ok = false;
        m_StatusCode = 408;
        m_ResponseBody = "";
        m_Done = true;
        TakaroLog.Debug("HTTP " + m_Tag + " timeout");
        OnComplete();
    }

    // Subclasses override this to receive the result.
    void OnComplete() {}
}

class TakaroHttpClient
{
    string m_BaseUrl;
    string m_AuthHeader;
    RestApi m_Api;
    int m_TimeoutSeconds;

    void TakaroHttpClient(string baseUrl, string identityToken, int timeoutSeconds)
    {
        m_BaseUrl = baseUrl;
        m_TimeoutSeconds = timeoutSeconds;
        if (identityToken != "")
            m_AuthHeader = "Bearer " + identityToken;
        else
            m_AuthHeader = "";

        m_Api = CreateRestApi();
        m_Api.EnableDebug(false);
    }

    void UpdateAuth(string identityToken)
    {
        if (identityToken != "")
            m_AuthHeader = "Bearer " + identityToken;
        else
            m_AuthHeader = "";
    }

    private RestContext NewContext()
    {
        RestContext ctx = m_Api.GetRestContext(m_BaseUrl);
        ctx.SetHeader("application/json");
        if (m_AuthHeader != "")
        {
            // DayZ RestContext.SetHeader takes a single header line; we keep the
            // last call as the Authorization header. If your build of DayZ supports
            // multi-header context, replace with explicit header method.
            ctx.SetHeader("application/json\r\nAuthorization: " + m_AuthHeader);
        }
        return ctx;
    }

    void Post(string path, string jsonBody, TakaroHttpCallback callback)
    {
        if (!callback)
            return;
        RestContext ctx = NewContext();
        TakaroLog.Debug("POST " + m_BaseUrl + path + " body=" + jsonBody.Substring(0, Math.Min(120, jsonBody.Length())));
        ctx.POST(callback, path, jsonBody);
    }

    void Get(string path, TakaroHttpCallback callback)
    {
        if (!callback)
            return;
        RestContext ctx = NewContext();
        TakaroLog.Debug("GET " + m_BaseUrl + path);
        ctx.GET(callback, path);
    }
}
