/**
 * Cloudflare Worker: OTA Firmware Proxy for Quectel SIM / Embedded Devices
 * 
 * Features:
 * - Fetches latest release artifacts from GitHub Releases.
 * - Handles GitHub 302 redirects internally on Cloudflare edge.
 * - Returns raw HTTP 200 with Content-Length to embedded devices (no redirect).
 * - Supports Private Repositories using Cloudflare Secret GITHUB_TOKEN.
 * - Ultra-low latency and zero infrastructure maintenance (Free 100,000 req/day).
 */

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const owner = env.GITHUB_OWNER || "hoanv-synaptix";
    const repo = env.GITHUB_REPO || "Charger";
    const token = env.GITHUB_TOKEN || "";

    const headers = {
      "User-Agent": "Cloudflare-OTA-Worker",
      "Accept": "application/vnd.github.v3+json"
    };
    if (token) {
      headers["Authorization"] = `Bearer ${token}`;
    }

    // Helper: Get release metadata from GitHub
    async function getRelease(tag) {
      const endpoint = tag 
        ? `https://api.github.com/repos/${owner}/${repo}/releases/tags/${tag}`
        : `https://api.github.com/repos/${owner}/${repo}/releases/latest`;
      
      const res = await fetch(endpoint, { headers });
      if (!res.ok) {
        throw new Error(`GitHub API error: ${res.status} ${res.statusText}`);
      }
      return await res.json();
    }

    try {
      // 1. Route: /manifest -> Returns ota_manifest.json directly
      if (url.pathname === "/manifest" || url.pathname === "/version.json") {
        const tag = url.searchParams.get("tag") || null;
        const release = await getRelease(tag);
        const manifestAsset = release.assets.find(a => a.name === "ota_manifest.json");

        if (!manifestAsset) {
          return new Response(JSON.stringify({ error: "ota_manifest.json not found in release" }), {
            status: 404,
            headers: { "Content-Type": "application/json" }
          });
        }

        const assetHeaders = { "User-Agent": "Cloudflare-OTA-Worker", "Accept": "application/octet-stream" };
        if (token) assetHeaders["Authorization"] = `Bearer ${token}`;

        const manifestRes = await fetch(manifestAsset.url, {
          headers: assetHeaders,
          redirect: "follow"
        });

        const manifestText = await manifestRes.text();
        return new Response(manifestText, {
          status: 200,
          headers: {
            "Content-Type": "application/json",
            "Access-Control-Allow-Origin": "*",
            "Cache-Control": "public, max-age=60"
          }
        });
      }

      // 2. Route: /firmware or /Charger.bin -> Streams binary with HTTP 200 (No 302 seen by device)
      if (url.pathname === "/firmware" || url.pathname === "/Charger.bin") {
        const tag = url.searchParams.get("tag") || null;
        const release = await getRelease(tag);
        const binAsset = release.assets.find(a => a.name === "Charger.bin");

        if (!binAsset) {
          return new Response("Charger.bin not found in release", { status: 404 });
        }

        const assetHeaders = {
          "User-Agent": "Cloudflare-OTA-Worker",
          "Accept": "application/octet-stream"
        };
        if (token) assetHeaders["Authorization"] = `Bearer ${token}`;

        // Fetch asset from GitHub (Worker follows GitHub -> S3 redirect internally)
        const binRes = await fetch(binAsset.url, {
          headers: assetHeaders,
          redirect: "follow"
        });

        if (!binRes.ok) {
          return new Response(`Failed to download binary: ${binRes.status}`, { status: 502 });
        }

        // Forward headers to modem
        const responseHeaders = new Headers();
        responseHeaders.set("Content-Type", "application/octet-stream");
        responseHeaders.set("Content-Disposition", `attachment; filename="Charger.bin"`);
        const len = binRes.headers.get("content-length");
        if (len) responseHeaders.set("Content-Length", len);

        return new Response(binRes.body, {
          status: 200,
          headers: responseHeaders
        });
      }

      // Default info page
      return new Response(JSON.stringify({
        service: "STM32 Charger OTA Proxy",
        repository: `${owner}/${repo}`,
        endpoints: {
          manifest: `${url.origin}/manifest`,
          firmware: `${url.origin}/firmware`
        }
      }, null, 2), {
        status: 200,
        headers: { "Content-Type": "application/json" }
      });

    } catch (err) {
      return new Response(JSON.stringify({ error: err.message }), {
        status: 500,
        headers: { "Content-Type": "application/json" }
      });
    }
  }
};
