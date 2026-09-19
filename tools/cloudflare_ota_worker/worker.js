/**
 * Cloudflare Worker: lean OTA proxy for Quectel embedded devices.
 *
 * The device talks only to this Worker. GitHub API redirects remain inside
 * the Worker, so the modem receives a normal HTTP 200 stream.
 */

const MANIFEST_NAME = "ota_manifest.json";
const FIRMWARE_NAME = "Charger.bin";
const DEFAULT_CACHE_TTL = 60;

function jsonResponse(body, status, extraHeaders = {}) {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      "Content-Type": "application/json; charset=utf-8",
      "Cache-Control": "no-store",
      ...extraHeaders
    }
  });
}

function configuredTag(url, env) {
  if (env.ALLOW_TAG_QUERY === "true") {
    const queryTag = url.searchParams.get("tag");
    if (queryTag) return queryTag;
  }
  return env.OTA_RELEASE_TAG || "";
}

function parseManifest(value) {
  let manifest;
  try {
    manifest = JSON.parse(value);
  } catch (_) {
    return { ok: false, error: "invalid_manifest" };
  }

  const target = manifest.target_mcu || manifest.target;
  const crc32 = manifest.crc32 || manifest.crc32_hex;
  const required = ["version", "version_code", "size", "sha256"];
  if (!required.every((key) => Object.prototype.hasOwnProperty.call(manifest, key)) ||
      target !== "STM32G0B1" ||
      manifest.filename !== FIRMWARE_NAME ||
      !Number.isInteger(manifest.version_code) ||
      !Number.isInteger(manifest.size) || manifest.size <= 0 ||
      typeof manifest.sha256 !== "string" || !/^[0-9a-f]{64}$/i.test(manifest.sha256) ||
      typeof crc32 !== "string" || !/^0x[0-9a-f]{8}$/i.test(crc32)) {
    return { ok: false, error: "invalid_manifest" };
  }

  return {
    ok: true,
    value: { ...manifest, target_mcu: target, crc32, filename: FIRMWARE_NAME }
  };
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const owner = env.GITHUB_OWNER || "hoanv-synaptix";
    const repo = env.GITHUB_REPO || "Charger";
    const tag = configuredTag(url, env);
    const parsedTtl = Number.parseInt(env.OTA_CACHE_TTL || DEFAULT_CACHE_TTL, 10);
    const cacheTtl = Number.isFinite(parsedTtl) && parsedTtl > 0 ? parsedTtl : DEFAULT_CACHE_TTL;

    if (request.method !== "GET") {
      return jsonResponse({ error: "method_not_allowed" }, 405, { Allow: "GET" });
    }
    if (!tag) {
      return jsonResponse({ error: "ota_release_not_configured" }, 503);
    }

    const githubHeaders = new Headers({
      "User-Agent": "Cloudflare-OTA-Worker",
      Accept: "application/vnd.github.v3+json"
    });
    const assetHeaders = new Headers({
      "User-Agent": "Cloudflare-OTA-Worker",
      Accept: "application/octet-stream"
    });
    if (env.GITHUB_TOKEN) {
      githubHeaders.set("Authorization", `Bearer ${env.GITHUB_TOKEN}`);
      assetHeaders.set("Authorization", `Bearer ${env.GITHUB_TOKEN}`);
    }

    const releaseUrl = `https://api.github.com/repos/${encodeURIComponent(owner)}/${encodeURIComponent(repo)}/releases/tags/${encodeURIComponent(tag)}`;
    const cacheKey = new Request(`${url.origin}/__release/${encodeURIComponent(owner)}/${encodeURIComponent(repo)}/${encodeURIComponent(tag)}`);

    async function getRelease() {
      const cached = await caches.default.match(cacheKey);
      if (cached) return cached.json();

      const response = await fetch(releaseUrl, { headers: githubHeaders });
      if (response.status === 404) throw new Error("release_not_found");
      if (!response.ok) throw new Error("github_release_unavailable");

      const release = await response.json();
      if (!Array.isArray(release.assets)) throw new Error("release_assets_missing");
      const cachedResponse = new Response(JSON.stringify(release), {
        headers: {
          "Content-Type": "application/json",
          "Cache-Control": `public, max-age=${cacheTtl}`
        }
      });
      ctx.waitUntil(caches.default.put(cacheKey, cachedResponse.clone()));
      return release;
    }

    async function getAsset(release, name) {
      const asset = release.assets.find((entry) => entry.name === name);
      if (!asset || typeof asset.url !== "string") {
        throw new Error(name === MANIFEST_NAME ? "manifest_not_found" : "firmware_not_found");
      }
      const response = await fetch(asset.url, { headers: assetHeaders, redirect: "follow" });
      if (!response.ok) throw new Error("github_asset_unavailable");
      return { asset, response };
    }

    try {
      if (url.pathname === "/manifest" || url.pathname === "/version.json") {
        const release = await getRelease();
        const { response } = await getAsset(release, MANIFEST_NAME);
        const parsed = parseManifest(await response.text());
        if (!parsed.ok) return jsonResponse({ error: parsed.error }, 502);
        return jsonResponse(parsed.value, 200, {
          "Access-Control-Allow-Origin": "*",
          "Cache-Control": `public, max-age=${cacheTtl}`
        });
      }

      if (url.pathname === "/firmware" || url.pathname === "/Charger.bin") {
        const release = await getRelease();
        const { asset, response } = await getAsset(release, FIRMWARE_NAME);
        if (!Number.isInteger(asset.size) || asset.size <= 0) {
          return jsonResponse({ error: "firmware_size_missing" }, 502);
        }
        const headers = new Headers({
          "Content-Type": "application/octet-stream",
          "Content-Disposition": `attachment; filename="${FIRMWARE_NAME}"`,
          "Cache-Control": "public, max-age=60",
          "Content-Length": response.headers.get("content-length") || String(asset.size)
        });
        return new Response(response.body, { status: 200, headers });
      }

      return jsonResponse({
        service: "STM32 Charger OTA Proxy",
        repository: `${owner}/${repo}`,
        release_tag: tag,
        endpoints: { manifest: `${url.origin}/manifest`, firmware: `${url.origin}/firmware` }
      }, 200, { "Cache-Control": "public, max-age=60" });
    } catch (error) {
      const message = error instanceof Error ? error.message : "internal_error";
      const status = message === "release_not_found" || message.endsWith("_not_found") ? 404 : 502;
      return jsonResponse({ error: message }, status);
    }
  }
};
