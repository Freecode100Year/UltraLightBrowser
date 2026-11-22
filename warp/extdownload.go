package main

// -download: fetches an extension for the settings page's "推荐扩展" list and
// checks it is the genuine file before the browser installs it.
//
//	ULB_DOWNLOAD         https URL of the .crx or .zip
//	ULB_DOWNLOAD_TO      where to save it
//	ULB_DOWNLOAD_CRXID   expected extension id (a CRX3 from the Chrome Web Store:
//	                     its RSA signature is verified and the signing key must
//	                     give this id), or
//	ULB_DOWNLOAD_SHA256  expected SHA-256 of the file (a release ZIP)
//
// Prints "DOWNLOADED <bytes>" or "ERROR download: <reason>". With -socks it goes
// only through that proxy: the browser never goes around its line, so neither
// does this download.

import (
	"bytes"
	"crypto"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/x509"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"strings"
	"time"
)

const maxExtension = 64 << 20

func runDownload(socks string) {
	target := strings.TrimSpace(os.Getenv("ULB_DOWNLOAD"))
	dest := os.Getenv("ULB_DOWNLOAD_TO")
	wantID := strings.ToLower(strings.TrimSpace(os.Getenv("ULB_DOWNLOAD_CRXID")))
	wantSum := strings.ToLower(strings.TrimSpace(os.Getenv("ULB_DOWNLOAD_SHA256")))
	if err := download(target, dest, wantID, wantSum, socks); err != nil {
		status("ERROR download: %v", err)
		os.Exit(1)
	}
}

func download(target, dest, wantID, wantSum, socks string) error {
	u, err := url.Parse(target)
	if err != nil || u.Scheme != "https" || u.Host == "" {
		return errors.New("下载地址必须是 https://")
	}
	if dest == "" || (wantID == "" && wantSum == "") {
		return errors.New("缺少保存位置或校验值")
	}
	var proxy *url.URL
	if socks != "" {
		proxy = &url.URL{Scheme: "socks5", Host: socks}
	}
	body, err := fetchFile(target, proxy)
	if err != nil {
		return err
	}
	if err := checkExtension(body, wantID, wantSum); err != nil {
		return err
	}
	tmp := dest + ".part"
	if err := os.WriteFile(tmp, body, 0o600); err != nil {
		return fmt.Errorf("无法保存：%v", err)
	}
	if err := os.Rename(tmp, dest); err != nil {
		os.Remove(tmp)
		return fmt.Errorf("无法保存：%v", err)
	}
	status("DOWNLOADED %d", len(body))
	return nil
}

func fetchFile(target string, proxy *url.URL) ([]byte, error) {
	tr := &http.Transport{Proxy: func(*http.Request) (*url.URL, error) { return proxy, nil }}
	defer tr.CloseIdleConnections()
	client := &http.Client{Transport: tr, Timeout: 120 * time.Second}
	resp, err := client.Get(target)
	if err != nil {
		return nil, fmt.Errorf("连接失败：%v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("服务器返回 %d", resp.StatusCode)
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, maxExtension+1))
	if err != nil {
		return nil, fmt.Errorf("下载中断：%v", err)
	}
	if len(body) > maxExtension {
		return nil, errors.New("文件太大")
	}
	return body, nil
}

func checkExtension(body []byte, wantID, wantSum string) error {
	if wantSum != "" {
		sum := sha256.Sum256(body)
		if hex.EncodeToString(sum[:]) != wantSum {
			return errors.New("文件校验失败（SHA-256 不符），已丢弃")
		}
		return nil
	}
	id, err := verifyCRX3(body)
	if err != nil {
		return err
	}
	if id != wantID {
		return fmt.Errorf("扩展 ID 不符（%s），已丢弃", id)
	}
	return nil
}

// verifyCRX3 checks a CRX3 file the way Chrome does: the signed header names the
// extension id, one RSA key must hash to that id, and that key's signature over
// the header and the ZIP must be valid. Returns the id (32 letters a-p).
func verifyCRX3(b []byte) (string, error) {
	bad := errors.New("不是有效的 CRX3 扩展包")
	if len(b) < 12 || string(b[:4]) != "Cr24" || binary.LittleEndian.Uint32(b[4:]) != 3 {
		return "", bad
	}
	hlen := int(binary.LittleEndian.Uint32(b[8:]))
	if hlen < 0 || 12+hlen > len(b) {
		return "", bad
	}
	header, archive := b[12:12+hlen], b[12+hlen:]
	if len(archive) < 4 || string(archive[:4]) != "PK\x03\x04" {
		return "", bad
	}
	var signed []byte
	type proof struct{ key, sig []byte }
	var proofs []proof
	err := protoFields(header, func(num int, val []byte) error {
		switch num {
		case 2: // sha256_with_rsa: AsymmetricKeyProof
			var p proof
			if err := protoFields(val, func(n int, v []byte) error {
				if n == 1 {
					p.key = v
				} else if n == 2 {
					p.sig = v
				}
				return nil
			}); err != nil {
				return err
			}
			proofs = append(proofs, p)
		case 10000: // signed_header_data: SignedData
			signed = val
		}
		return nil
	})
	if err != nil || signed == nil {
		return "", bad
	}
	var crxID []byte
	if err := protoFields(signed, func(n int, v []byte) error {
		if n == 1 {
			crxID = v
		}
		return nil
	}); err != nil || len(crxID) != 16 {
		return "", bad
	}
	msg := sha256.New()
	msg.Write([]byte("CRX3 SignedData\x00"))
	var n [4]byte
	binary.LittleEndian.PutUint32(n[:], uint32(len(signed)))
	msg.Write(n[:])
	msg.Write(signed)
	msg.Write(archive)
	digest := msg.Sum(nil)
	for _, p := range proofs {
		h := sha256.Sum256(p.key)
		if !bytes.Equal(h[:16], crxID) {
			continue
		}
		pub, err := x509.ParsePKIXPublicKey(p.key)
		if err != nil {
			continue
		}
		rk, ok := pub.(*rsa.PublicKey)
		if !ok {
			continue
		}
		if rsa.VerifyPKCS1v15(rk, crypto.SHA256, digest, p.sig) == nil {
			return extensionID(crxID), nil
		}
		return "", errors.New("扩展包签名无效，已丢弃")
	}
	return "", errors.New("扩展包没有开发者签名，已丢弃")
}

// extensionID writes 16 bytes the way Chrome names extensions: hex digits 0-f as letters a-p.
func extensionID(raw []byte) string {
	var sb strings.Builder
	for _, c := range hex.EncodeToString(raw) {
		if c >= 'a' {
			sb.WriteByte(byte('a' + 10 + c - 'a'))
		} else {
			sb.WriteByte(byte('a' + c - '0'))
		}
	}
	return sb.String()
}

// protoFields walks a protobuf message, calling fn for length-delimited fields
// and skipping the other wire types.
func protoFields(b []byte, fn func(num int, val []byte) error) error {
	bad := errors.New("bad protobuf")
	for len(b) > 0 {
		tag, n := binary.Uvarint(b)
		if n <= 0 {
			return bad
		}
		b = b[n:]
		switch tag & 7 {
		case 0:
			_, n := binary.Uvarint(b)
			if n <= 0 {
				return bad
			}
			b = b[n:]
		case 1:
			if len(b) < 8 {
				return bad
			}
			b = b[8:]
		case 5:
			if len(b) < 4 {
				return bad
			}
			b = b[4:]
		case 2:
			l, n := binary.Uvarint(b)
			if n <= 0 || l > uint64(len(b)-n) {
				return bad
			}
			if err := fn(int(tag>>3), b[n:n+int(l)]); err != nil {
				return err
			}
			b = b[n+int(l):]
		default:
			return bad
		}
	}
	return nil
}
