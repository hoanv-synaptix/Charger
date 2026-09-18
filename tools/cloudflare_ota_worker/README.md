# Hướng dẫn Triển khai Cloudflare Worker OTA Proxy

Cloudflare Worker đóng vai trò là một **Proxy trung gian** giúp module SIM 4G (Quectel) tải firmware trực tiếp từ GitHub Releases mà **không bị lỗi chuyển hướng HTTP 302**, hỗ trợ cả repository Private một cách bảo mật.

---

## Cách 1: Triển khai qua Cloudflare Dashboard (Không cần cài Node.js, chỉ mất 2 phút)

1. Đăng nhập vào [Cloudflare Dashboard](https://dash.cloudflare.com/) (miễn phí).
2. Vào mục **Compute (Workers & Pages)** $\rightarrow$ Bấm **Create application** $\rightarrow$ **Create Worker**.
3. Đặt tên worker: `charger-ota` $\rightarrow$ Bấm **Deploy**.
4. Sau khi deploy xong, bấm **Edit code**:
   - Xóa toàn bộ code mặc định.
   - Copy toàn bộ nội dung file `worker.js` dán vào.
   - Bấm **Deploy**.
5. Cấu hình biến môi trường (nếu Repo là Private):
   - Vào tab **Settings** của Worker $\rightarrow$ **Variables and Secrets**.
   - Bấm **Add variable**:
     - `GITHUB_OWNER` = `hoanv-synaptix`
     - `GITHUB_REPO` = `Charger`
   - Bấm **Add secret**:
     - `GITHUB_TOKEN` = `<Personal_Access_Token_GitHub_Của_Bạn>` (có quyền `repo` để đọc release private).
   - Bấm **Save and Deploy**.

---

## Cách 2: Triển khai qua lệnh Wrangler CLI
```bash
cd Charger/tools/cloudflare_ota_worker
npx wrangler deploy
# Nếu repo là private:
npx wrangler secret put GITHUB_TOKEN
```

---

## Các Endpoint dành cho Trạm Sạc / Module SIM

Giả sử domain worker của bạn là `https://charger-ota.<your-subdomain>.workers.dev` (hoặc custom domain của bạn):

1. **Lấy thông tin phiên bản mới nhất**:
   ```
   GET https://charger-ota.<your-subdomain>.workers.dev/manifest
   ```
   *Kết quả trả về JSON*:
   ```json
   {
     "target": "STM32G0B1",
     "filename": "Charger.bin",
     "version": "1.0.0",
     "version_code": 65536,
     "size": 94220,
     "crc32_hex": "0x382AC1D5",
     "crc32_dec": 942326229
   }
   ```

2. **Tải file nhị phân Firmware trực tiếp**:
   ```
   GET https://charger-ota.<your-subdomain>.workers.dev/firmware
   ```
   - Cloudflare Worker sẽ tự tải từ GitHub CDN và stream thẳng về cho module Quectel với **HTTP 200 OK**.
   - **Không bị Redirect 302** $\rightarrow$ Module SIM Quectel chạy lệnh `AT+QHTTPGET` và `AT+QHTTPREAD` mượt mà 100%.
