package main

import (
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/x509"
	"encoding/binary"
	"encoding/hex"
	"strings"
	"testing"
)

func pbField(num int, val []byte) []byte {
	out := binary.AppendUvarint(nil, uint64(num)<<3|2)
	out = binary.AppendUvarint(out, uint64(len(val)))
	return append(out, val...)
}

// makeCRX3 builds a signed CRX3 around zip, the way the Chrome Web Store does.
func makeCRX3(t *testing.T, key *rsa.PrivateKey, zip []byte) ([]byte, string) {
	t.Helper()
	pub, err := x509.MarshalPKIXPublicKey(&key.PublicKey)
	if err != nil {
		t.Fatal(err)
	}
	h := sha256.Sum256(pub)
	signed := pbField(1, h[:16])
	msg := sha256.New()
	msg.Write([]byte("CRX3 SignedData\x00"))
	msg.Write(binary.LittleEndian.AppendUint32(nil, uint32(len(signed))))
	msg.Write(signed)
	msg.Write(zip)
	sig, err := rsa.SignPKCS1v15(rand.Reader, key, crypto.SHA256, msg.Sum(nil))
	if err != nil {
		t.Fatal(err)
	}
	header := append(pbField(2, append(pbField(1, pub), pbField(2, sig)...)), pbField(10000, signed)...)
	out := []byte("Cr24")
	out = binary.LittleEndian.AppendUint32(out, 3)
	out = binary.LittleEndian.AppendUint32(out, uint32(len(header)))
	out = append(append(out, header...), zip...)
	return out, extensionID(h[:16])
}

func TestVerifyCRX3(t *testing.T) {
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	zip := []byte("PK\x03\x04 pretend archive")
	crx, id := makeCRX3(t, key, zip)
	got, err := verifyCRX3(crx)
	if err != nil || got != id {
		t.Fatalf("verify = %q, %v; want %q", got, err, id)
	}
	if err := checkExtension(crx, id, ""); err != nil {
		t.Fatal(err)
	}
	if err := checkExtension(crx, strings.Repeat("a", 32), ""); err == nil {
		t.Fatal("wrong id accepted")
	}
	tampered := append([]byte(nil), crx...)
	tampered[len(tampered)-1] ^= 1
	if _, err := verifyCRX3(tampered); err == nil {
		t.Fatal("tampered archive accepted")
	}
	// Signed by another key that does not match the id in the header.
	other, _ := rsa.GenerateKey(rand.Reader, 2048)
	forged, _ := makeCRX3(t, other, zip)
	if _, err := verifyCRX3(forged); err != nil {
		t.Fatalf("a self-consistent CRX from another key should verify (its id differs): %v", err)
	}
	if err := checkExtension(forged, id, ""); err == nil {
		t.Fatal("CRX from another developer accepted")
	}
	for _, junk := range [][]byte{nil, []byte("Cr24"), []byte("PK\x03\x04abc"), append([]byte("Cr24\x03\x00\x00\x00\xff\xff\x00\x00"), zip...)} {
		if _, err := verifyCRX3(junk); err == nil {
			t.Fatalf("junk %q accepted", junk)
		}
	}
}

func TestCheckSHA256(t *testing.T) {
	body := []byte("PK\x03\x04 release zip")
	sum := sha256.Sum256(body)
	if err := checkExtension(body, "", hex.EncodeToString(sum[:])); err != nil {
		t.Fatal(err)
	}
	if err := checkExtension(append(body, 'x'), "", hex.EncodeToString(sum[:])); err == nil {
		t.Fatal("changed file accepted")
	}
}

func TestExtensionID(t *testing.T) {
	raw, _ := hex.DecodeString("0123456789abcdef0123456789abcdef")
	if got := extensionID(raw); got != "abcdefghijklmnopabcdefghijklmnop" {
		t.Fatal(got)
	}
}

func TestDownloadRejects(t *testing.T) {
	for _, c := range [][4]string{
		{"http://example.com/x.crx", "/tmp/x", "abc", ""},
		{"https://example.com/x.crx", "", "abc", ""},
		{"https://example.com/x.crx", "/tmp/x", "", ""},
	} {
		if err := download(c[0], c[1], c[2], c[3], ""); err == nil {
			t.Fatalf("%v accepted", c)
		}
	}
}
