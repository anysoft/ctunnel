const API_VERSION = "v1";
const API_MODULE = "ddns";
const KV_PREFIX = "ddns:v1";
const SCHEMA_VERSION = 1;
const MAX_REPORT_BODY_BYTES = 2048;

/**
 * 统一 JSON 返回。
 */
function jsonResponse(
  requestId,
  status,
  code,
  data = null,
  message = null,
) {
  const body = {
    ok: status >= 200 && status < 300,
    code,
  };

  if (data !== null) {
    body.data = data;
  }

  if (message !== null) {
    body.message = message;
  }

  body.request_id = requestId;

  return new Response(JSON.stringify(body), {
    status,
    headers: {
      "Content-Type": "application/json; charset=utf-8",
      "Cache-Control": "no-store",
      "X-Content-Type-Options": "nosniff",
      "X-Robots-Tag": "noindex, nofollow",
    },
  });
}

/**
 * 纯文本返回，主要供 Shell 解析。
 */
function textResponse(status, text) {
  return new Response(`${text}\n`, {
    status,
    headers: {
      "Content-Type": "text/plain; charset=utf-8",
      "Cache-Control": "no-store",
      "X-Content-Type-Options": "nosniff",
      "X-Robots-Tag": "noindex, nofollow",
    },
  });
}

/**
 * Bearer Token 鉴权。
 */
function authorized(request, expectedToken) {
  if (!expectedToken) {
    return false;
  }

  const authorization =
    request.headers.get("Authorization");

  return authorization === `Bearer ${expectedToken}`;
}

/**
 * 检查路径段，防止任意字符进入 KV Key。
 */
function validPathSegment(value) {
  return /^[a-zA-Z0-9][a-zA-Z0-9._-]{0,63}$/.test(
    value,
  );
}

/**
 * 支持：
 *
 * /v1/ddns/report/home/router
 * /v1/ddns/resolve/home/router
 *
 * 最多允许四层记录 ID：
 *
 * /v1/ddns/report/home/china/primary/router
 */
function parseRoute(pathname) {
  const parts = pathname
    .split("/")
    .filter(Boolean)
    .map((part) => {
      try {
        return decodeURIComponent(part);
      } catch {
        return "";
      }
    });

  if (
    parts.length < 4 ||
    parts.length > 7 ||
    parts[0] !== API_VERSION ||
    parts[1] !== API_MODULE
  ) {
    return null;
  }

  const action = parts[2];

  if (
    action !== "report" &&
    action !== "resolve"
  ) {
    return null;
  }

  const idParts = parts.slice(3);

  if (!idParts.every(validPathSegment)) {
    return null;
  }

  return {
    action,
    id: idParts.join("/"),
    kvKey: `${KV_PREFIX}:${idParts.join(":")}`,
  };
}

/**
 * 验证并规范化 IPv4。
 */
function canonicalizeIPv4(value) {
  if (typeof value !== "string") {
    return null;
  }

  const input = value.trim();
  const parts = input.split(".");

  if (parts.length !== 4) {
    return null;
  }

  const normalized = [];

  for (const part of parts) {
    if (!/^(0|[1-9][0-9]{0,2})$/.test(part)) {
      return null;
    }

    const number = Number(part);

    if (
      !Number.isInteger(number) ||
      number < 0 ||
      number > 255
    ) {
      return null;
    }

    normalized.push(String(number));
  }

  return normalized.join(".");
}

/**
 * 使用 URL 解析器验证并规范化 IPv6。
 *
 * 不支持带 zone ID 的地址，例如 fe80::1%eth0；
 * DDNS 本身也不应该保存链路本地地址。
 */
function canonicalizeIPv6(value) {
  if (typeof value !== "string") {
    return null;
  }

  const input = value.trim().toLowerCase();

  if (
    input.length === 0 ||
    !input.includes(":") ||
    input.includes("%") ||
    !/^[0-9a-f:.]+$/.test(input)
  ) {
    return null;
  }

  try {
    const parsed = new URL(`http://[${input}]/`);
    let hostname = parsed.hostname;

    if (
      hostname.startsWith("[") &&
      hostname.endsWith("]")
    ) {
      hostname = hostname.slice(1, -1);
    }

    return hostname.includes(":")
      ? hostname.toLowerCase()
      : null;
  } catch {
    return null;
  }
}

/**
 * 记录 Cloudflare 看到的本次上报来源地址，仅用于排查。
 *
 * 真正保存的 IPv4、IPv6 来自客户端提交的完整快照。
 */
function getObservedAddress(request) {
  return (
    request.headers.get("CF-Connecting-IPv6") ||
    request.headers.get("CF-Connecting-IP") ||
    null
  );
}

/**
 * 从 KV 记录读取指定协议族地址。
 */
function getStoredAddress(record, family) {
  const entry = record?.addresses?.[family];

  if (!entry || typeof entry !== "object") {
    return null;
  }

  return typeof entry.address === "string"
    ? entry.address
    : null;
}

/**
 * 如果地址未变化，保留原来的 updated_at。
 */
function buildAddressEntry(
  previousEntry,
  address,
  now,
) {
  if (address === null) {
    return null;
  }

  if (
    previousEntry &&
    typeof previousEntry === "object" &&
    previousEntry.address === address &&
    typeof previousEntry.updated_at === "string"
  ) {
    return previousEntry;
  }

  return {
    address,
    updated_at: now,
  };
}

/**
 * 解析完整地址快照。
 *
 * 请求必须同时包含：
 *
 * {
 *   "addresses": {
 *     "ipv4": "1.2.3.4" 或 null,
 *     "ipv6": "2001:db8::1" 或 null
 *   }
 * }
 */
async function parseReportBody(
  request,
  requestId,
) {
  const contentType =
    request.headers.get("Content-Type") || "";

  if (
    !contentType
      .toLowerCase()
      .startsWith("application/json")
  ) {
    return {
      error: jsonResponse(
        requestId,
        415,
        "UNSUPPORTED_MEDIA_TYPE",
        null,
        "Content-Type must be application/json",
      ),
    };
  }

  const declaredLength = Number(
    request.headers.get("Content-Length") || 0,
  );

  if (
    declaredLength > MAX_REPORT_BODY_BYTES
  ) {
    return {
      error: jsonResponse(
        requestId,
        413,
        "REPORT_BODY_TOO_LARGE",
        null,
        "Request body is too large",
      ),
    };
  }

  const rawBody = await request.text();

  if (
    new TextEncoder().encode(rawBody).byteLength >
    MAX_REPORT_BODY_BYTES
  ) {
    return {
      error: jsonResponse(
        requestId,
        413,
        "REPORT_BODY_TOO_LARGE",
        null,
        "Request body is too large",
      ),
    };
  }

  let body;

  try {
    body = JSON.parse(rawBody);
  } catch {
    return {
      error: jsonResponse(
        requestId,
        400,
        "INVALID_JSON",
        null,
        "Invalid JSON body",
      ),
    };
  }

  if (
    !body ||
    typeof body !== "object" ||
    Array.isArray(body)
  ) {
    return {
      error: jsonResponse(
        requestId,
        400,
        "INVALID_REPORT_BODY",
        null,
        "Request body must be a JSON object",
      ),
    };
  }

  const addresses = body.addresses;

  if (
    !addresses ||
    typeof addresses !== "object" ||
    Array.isArray(addresses)
  ) {
    return {
      error: jsonResponse(
        requestId,
        400,
        "INVALID_ADDRESSES",
        null,
        "addresses must be a JSON object",
      ),
    };
  }

  const hasIpv4 =
    Object.prototype.hasOwnProperty.call(
      addresses,
      "ipv4",
    );

  const hasIpv6 =
    Object.prototype.hasOwnProperty.call(
      addresses,
      "ipv6",
    );

  /*
   * 这是完整快照 PUT。
   *
   * 两个字段都必须出现；没有某个协议族时必须传 null，
   * 防止因客户端漏字段而意外保留旧地址。
   */
  if (!hasIpv4 || !hasIpv6) {
    return {
      error: jsonResponse(
        requestId,
        400,
        "INCOMPLETE_ADDRESS_SNAPSHOT",
        null,
        "addresses.ipv4 and addresses.ipv6 are both required; use null when unavailable",
      ),
    };
  }

  let ipv4 = null;
  let ipv6 = null;

  if (addresses.ipv4 !== null) {
    ipv4 = canonicalizeIPv4(addresses.ipv4);

    if (ipv4 === null) {
      return {
        error: jsonResponse(
          requestId,
          400,
          "INVALID_IPV4",
          null,
          "addresses.ipv4 is invalid",
        ),
      };
    }
  }

  if (addresses.ipv6 !== null) {
    ipv6 = canonicalizeIPv6(addresses.ipv6);

    if (ipv6 === null) {
      return {
        error: jsonResponse(
          requestId,
          400,
          "INVALID_IPV6",
          null,
          "addresses.ipv6 is invalid",
        ),
      };
    }
  }

  if (ipv4 === null && ipv6 === null) {
    return {
      error: jsonResponse(
        requestId,
        400,
        "ADDRESS_REQUIRED",
        null,
        "At least one public address is required",
      ),
    };
  }

  return {
    snapshot: {
      ipv4,
      ipv6,
    },
  };
}

/**
 * PUT /v1/ddns/report/{record}
 */
async function handleReport(
  request,
  env,
  route,
  requestId,
) {
  if (request.method !== "PUT") {
    return jsonResponse(
      requestId,
      405,
      "METHOD_NOT_ALLOWED",
      null,
      "Use PUT",
    );
  }

  if (
    !authorized(request, env.WRITE_TOKEN)
  ) {
    return jsonResponse(
      requestId,
      401,
      "UNAUTHORIZED",
      null,
      "Unauthorized",
    );
  }

  const parsedBody = await parseReportBody(
    request,
    requestId,
  );

  if (parsedBody.error) {
    return parsedBody.error;
  }

  const snapshot = parsedBody.snapshot;

  const previous = await env.ADDR_KV.get(
    route.kvKey,
    "json",
  );

  const previousIpv4 =
    getStoredAddress(previous, "ipv4");

  const previousIpv6 =
    getStoredAddress(previous, "ipv6");

  const changedIpv4 =
    previousIpv4 !== snapshot.ipv4;

  const changedIpv6 =
    previousIpv6 !== snapshot.ipv6;

  const changed =
    changedIpv4 || changedIpv6;

  /*
   * 地址没有变化时不写 KV。
   */
  if (!changed && previous) {
    return jsonResponse(
      requestId,
      200,
      "DDNS_REPORT_OK",
      {
        id: route.id,
        status: "unchanged",
        changed: false,
        changed_families: [],
        revision: Number(
          previous.revision || 0,
        ),
        updated_at:
          previous.updated_at || null,
      },
    );
  }

  const now = new Date().toISOString();

  const record = {
    schema_version: SCHEMA_VERSION,
    id: route.id,
    revision:
      Number(previous?.revision || 0) + 1,

    addresses: {
      ipv4: buildAddressEntry(
        previous?.addresses?.ipv4,
        snapshot.ipv4,
        now,
      ),

      ipv6: buildAddressEntry(
        previous?.addresses?.ipv6,
        snapshot.ipv6,
        now,
      ),
    },

    /*
     * 只用于排查这次 PUT 是从哪个地址到达 Cloudflare。
     * 不参与 DDNS 地址解析。
     */
    last_report: {
      observed_address:
        getObservedAddress(request),
      reported_at: now,
    },

    created_at:
      previous?.created_at || now,

    updated_at: now,
  };

  await env.ADDR_KV.put(
    route.kvKey,
    JSON.stringify(record),
  );

  const changedFamilies = [];

  if (changedIpv4) {
    changedFamilies.push("ipv4");
  }

  if (changedIpv6) {
    changedFamilies.push("ipv6");
  }

  return jsonResponse(
    requestId,
    previous ? 200 : 201,
    "DDNS_REPORT_OK",
    {
      id: route.id,
      status: previous
        ? "updated"
        : "created",
      changed: true,
      changed_families: changedFamilies,
      revision: record.revision,
      updated_at: record.updated_at,
    },
  );
}

function selectAddress(record, family) {
  if (family === "ipv4") {
    return getStoredAddress(
      record,
      "ipv4",
    );
  }

  if (family === "ipv6") {
    return getStoredAddress(
      record,
      "ipv6",
    );
  }

  if (family === "auto") {
    return (
      getStoredAddress(record, "ipv6") ||
      getStoredAddress(record, "ipv4")
    );
  }

  return undefined;
}

/**
 * GET /v1/ddns/resolve/{record}
 *
 * 默认返回完整 JSON。
 *
 * ?format=text&family=ipv6
 * ?format=text&family=ipv4
 * ?format=text&family=auto
 */
async function handleResolve(
  request,
  env,
  route,
  requestId,
  url,
) {
  if (request.method !== "GET") {
    return jsonResponse(
      requestId,
      405,
      "METHOD_NOT_ALLOWED",
      null,
      "Use GET",
    );
  }

  if (
    !authorized(request, env.READ_TOKEN)
  ) {
    return jsonResponse(
      requestId,
      401,
      "UNAUTHORIZED",
      null,
      "Unauthorized",
    );
  }

  const record = await env.ADDR_KV.get(
    route.kvKey,
    "json",
  );

  if (!record) {
    return jsonResponse(
      requestId,
      404,
      "DDNS_RECORD_NOT_FOUND",
      null,
      "DDNS record not found",
    );
  }

  const format = (
    url.searchParams.get("format") ||
    "json"
  ).toLowerCase();

  if (format === "json") {
    return jsonResponse(
      requestId,
      200,
      "DDNS_RESOLVE_OK",
      record,
    );
  }

  if (format !== "text") {
    return jsonResponse(
      requestId,
      400,
      "INVALID_FORMAT",
      null,
      "format must be json or text",
    );
  }

  const family = (
    url.searchParams.get("family") ||
    "auto"
  ).toLowerCase();

  const address = selectAddress(
    record,
    family,
  );

  if (address === undefined) {
    return jsonResponse(
      requestId,
      400,
      "INVALID_ADDRESS_FAMILY",
      null,
      "family must be ipv4, ipv6, or auto",
    );
  }

  if (!address) {
    return jsonResponse(
      requestId,
      404,
      "ADDRESS_FAMILY_NOT_FOUND",
      null,
      `No ${family} address is available`,
    );
  }

  return textResponse(200, address);
}

export default {
  async fetch(request, env) {
    const requestId =
      crypto.randomUUID();

    const url = new URL(request.url);

    /*
     * 健康检查不访问 KV。
     */
    if (
      url.pathname === "/health" ||
      url.pathname === "/v1/health"
    ) {
      return jsonResponse(
        requestId,
        200,
        "SERVICE_OK",
        {
          service: "private-ddns",
          version: 1,
        },
      );
    }

    const route = parseRoute(
      url.pathname,
    );

    if (!route) {
      return jsonResponse(
        requestId,
        404,
        "ROUTE_NOT_FOUND",
        null,
        "Route not found",
      );
    }

    try {
      if (route.action === "report") {
        return await handleReport(
          request,
          env,
          route,
          requestId,
        );
      }

      return await handleResolve(
        request,
        env,
        route,
        requestId,
        url,
      );
    } catch (error) {
      console.error(
        "Worker request failed",
        {
          requestId,
          error:
            error instanceof Error
              ? error.stack
              : String(error),
        },
      );

      return jsonResponse(
        requestId,
        500,
        "INTERNAL_ERROR",
        null,
        "Internal server error",
      );
    }
  },
};
