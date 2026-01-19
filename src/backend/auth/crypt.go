package auth

import (
	"bytes"
	"crypto/md5"
	"crypto/sha256"
	"crypto/sha512"
	"errors"
	"strconv"
	"strings"
)

const (
	md5Prefix    = "$1$"
	sha256Prefix = "$5$"
	sha512Prefix = "$6$"
)

const (
	shaRoundsMin     = 1000
	shaRoundsMax     = 999999999
	shaRoundsDefault = 5000
)

func cryptPassword(password, salt string) (string, error) {
	if strings.HasPrefix(salt, md5Prefix) {
		return md5Crypt(password, salt)
	}
	if strings.HasPrefix(salt, sha256Prefix) {
		return sha256Crypt(password, salt)
	}
	if strings.HasPrefix(salt, sha512Prefix) {
		return sha512Crypt(password, salt)
	}
	return "", errors.New("unsupported hash format")
}

func md5Crypt(password, salt string) (string, error) {
	saltBytes, err := extractSalt(md5Prefix, salt, 8)
	if err != nil {
		return "", err
	}
	key := []byte(password)

	alternate := md5.New()
	alternate.Write(key)
	alternate.Write(saltBytes)
	alternate.Write(key)
	alternateSum := alternate.Sum(nil)

	a := md5.New()
	a.Write(key)
	a.Write([]byte(md5Prefix))
	a.Write(saltBytes)

	i := len(key)
	for ; i > 16; i -= 16 {
		a.Write(alternateSum)
	}
	a.Write(alternateSum[0:i])

	for i = len(key); i > 0; i >>= 1 {
		if (i & 1) == 0 {
			a.Write(key[0:1])
		} else {
			a.Write([]byte{0})
		}
	}
	csum := a.Sum(nil)

	for i = 0; i < 1000; i++ {
		c := md5.New()
		if (i & 1) != 0 {
			c.Write(key)
		} else {
			c.Write(csum)
		}
		if i%3 != 0 {
			c.Write(saltBytes)
		}
		if i%7 != 0 {
			c.Write(key)
		}
		if (i & 1) == 0 {
			c.Write(key)
		} else {
			c.Write(csum)
		}
		csum = c.Sum(nil)
	}

	out := make([]byte, 0, 23+len(md5Prefix)+len(saltBytes))
	out = append(out, md5Prefix...)
	out = append(out, saltBytes...)
	out = append(out, '$')
	out = append(out, base64_24bit([]byte{
		csum[12], csum[6], csum[0],
		csum[13], csum[7], csum[1],
		csum[14], csum[8], csum[2],
		csum[15], csum[9], csum[3],
		csum[5], csum[10], csum[4],
		csum[11],
	})...)

	return string(out), nil
}

func sha256Crypt(password, salt string) (string, error) {
	return shaCrypt(password, salt, sha256Prefix)
}

func sha512Crypt(password, salt string) (string, error) {
	return shaCrypt(password, salt, sha512Prefix)
}

func shaCrypt(password, salt, prefix string) (string, error) {
	saltBytes, rounds, customRounds, err := extractSaltAndRounds(prefix, salt)
	if err != nil {
		return "", err
	}
	key := []byte(password)

	var digest func() hash
	var digestSize int
	if prefix == sha256Prefix {
		digest = func() hash { return sha256.New() }
		digestSize = sha256.Size
	} else {
		digest = func() hash { return sha512.New() }
		digestSize = sha512.Size
	}

	alternate := digest()
	alternate.Write(key)
	alternate.Write(saltBytes)
	alternate.Write(key)
	alternateSum := alternate.Sum(nil)

	a := digest()
	a.Write(key)
	a.Write(saltBytes)
	remaining := len(key)
	for remaining > digestSize {
		a.Write(alternateSum)
		remaining -= digestSize
	}
	a.Write(alternateSum[0:remaining])

	for i := len(key); i > 0; i >>= 1 {
		if (i & 1) != 0 {
			a.Write(alternateSum)
		} else {
			a.Write(key)
		}
	}
	asum := a.Sum(nil)

	p := digest()
	for i := 0; i < len(key); i++ {
		p.Write(key)
	}
	psum := p.Sum(nil)
	pseq := make([]byte, 0, len(key))
	remaining = len(key)
	for remaining > digestSize {
		pseq = append(pseq, psum...)
		remaining -= digestSize
	}
	pseq = append(pseq, psum[0:remaining]...)

	s := digest()
	for i := 0; i < (16 + int(asum[0])); i++ {
		s.Write(saltBytes)
	}
	ssum := s.Sum(nil)
	sseq := make([]byte, 0, len(saltBytes))
	remaining = len(saltBytes)
	for remaining > digestSize {
		sseq = append(sseq, ssum...)
		remaining -= digestSize
	}
	sseq = append(sseq, ssum[0:remaining]...)

	csum := asum
	for i := 0; i < rounds; i++ {
		c := digest()
		if (i & 1) != 0 {
			c.Write(pseq)
		} else {
			c.Write(csum)
		}
		if i%3 != 0 {
			c.Write(sseq)
		}
		if i%7 != 0 {
			c.Write(pseq)
		}
		if (i & 1) != 0 {
			c.Write(csum)
		} else {
			c.Write(pseq)
		}
		csum = c.Sum(nil)
	}

	out := make([]byte, 0, 123)
	out = append(out, prefix...)
	if customRounds {
		out = append(out, []byte("rounds="+strconv.Itoa(rounds)+"$")...)
	}
	out = append(out, saltBytes...)
	out = append(out, '$')
	if prefix == sha256Prefix {
		out = append(out, base64_24bit([]byte{
			csum[20], csum[10], csum[0],
			csum[11], csum[1], csum[21],
			csum[2], csum[22], csum[12],
			csum[23], csum[13], csum[3],
			csum[14], csum[4], csum[24],
			csum[5], csum[25], csum[15],
			csum[26], csum[16], csum[6],
			csum[17], csum[7], csum[27],
			csum[8], csum[28], csum[18],
			csum[29], csum[19], csum[9],
			csum[30], csum[31],
		})...)
	} else {
		out = append(out, base64_24bit([]byte{
			csum[42], csum[21], csum[0],
			csum[1], csum[43], csum[22],
			csum[23], csum[2], csum[44],
			csum[45], csum[24], csum[3],
			csum[4], csum[46], csum[25],
			csum[26], csum[5], csum[47],
			csum[48], csum[27], csum[6],
			csum[7], csum[49], csum[28],
			csum[29], csum[8], csum[50],
			csum[51], csum[30], csum[9],
			csum[10], csum[52], csum[31],
			csum[32], csum[11], csum[53],
			csum[54], csum[33], csum[12],
			csum[13], csum[55], csum[34],
			csum[35], csum[14], csum[56],
			csum[57], csum[36], csum[15],
			csum[16], csum[58], csum[37],
			csum[38], csum[17], csum[59],
			csum[60], csum[39], csum[18],
			csum[19], csum[61], csum[40],
			csum[41], csum[20], csum[62],
			csum[63],
		})...)
	}
	return string(out), nil
}

type hash interface {
	Write(p []byte) (n int, err error)
	Sum(b []byte) []byte
}

func extractSalt(prefix, salt string, maxLen int) ([]byte, error) {
	if !strings.HasPrefix(salt, prefix) {
		return nil, errors.New("invalid salt prefix")
	}
	saltToks := bytes.Split([]byte(salt), []byte{'$'})
	if len(saltToks) < 3 {
		return nil, errors.New("invalid salt format")
	}
	payload := saltToks[2]
	if len(payload) > maxLen {
		payload = payload[0:maxLen]
	}
	return payload, nil
}

func extractSaltAndRounds(prefix, salt string) ([]byte, int, bool, error) {
	if !strings.HasPrefix(salt, prefix) {
		return nil, 0, false, errors.New("invalid salt prefix")
	}
	saltToks := bytes.Split([]byte(salt), []byte{'$'})
	if len(saltToks) < 3 {
		return nil, 0, false, errors.New("invalid salt format")
	}
	var rounds int
	var custom bool
	payload := saltToks[2]
	if bytes.HasPrefix(payload, []byte("rounds=")) {
		custom = true
		parsed, err := strconv.Atoi(string(payload[7:]))
		if err != nil {
			return nil, 0, false, errors.New("invalid rounds")
		}
		rounds = parsed
		if rounds < shaRoundsMin {
			rounds = shaRoundsMin
		} else if rounds > shaRoundsMax {
			rounds = shaRoundsMax
		}
		if len(saltToks) < 4 {
			return nil, 0, false, errors.New("invalid salt format")
		}
		payload = saltToks[3]
	} else {
		rounds = shaRoundsDefault
	}
	if len(payload) > 16 {
		payload = payload[0:16]
	}
	return payload, rounds, custom, nil
}

func base64_24bit(src []byte) []byte {
	if len(src) == 0 {
		return []byte{}
	}

	const alphabet = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
	hashSize := (len(src) * 8) / 6
	if (len(src) % 6) != 0 {
		hashSize += 1
	}
	hash := make([]byte, hashSize)

	dst := hash
	for len(src) > 0 {
		switch len(src) {
		default:
			dst[0] = alphabet[src[0]&0x3f]
			dst[1] = alphabet[((src[0]>>6)|(src[1]<<2))&0x3f]
			dst[2] = alphabet[((src[1]>>4)|(src[2]<<4))&0x3f]
			dst[3] = alphabet[(src[2]>>2)&0x3f]
			src = src[3:]
			dst = dst[4:]
		case 2:
			dst[0] = alphabet[src[0]&0x3f]
			dst[1] = alphabet[((src[0]>>6)|(src[1]<<2))&0x3f]
			dst[2] = alphabet[(src[1]>>4)&0x3f]
			src = src[2:]
			dst = dst[3:]
		case 1:
			dst[0] = alphabet[src[0]&0x3f]
			dst[1] = alphabet[(src[0]>>6)&0x3f]
			src = src[1:]
			dst = dst[2:]
		}
	}

	return hash
}
