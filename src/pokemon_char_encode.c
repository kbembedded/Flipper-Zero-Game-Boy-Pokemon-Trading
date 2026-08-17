#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <src/include/pokemon_char_encode.h>

/* NOTE: These map to the Gen 1 character set! */
/* NOTE: These map to English */
/* TODO: It may make more sense to put this in a const array as a LUT,
 * e.g. t['F'], t['l'], t['i'], t['p'], t['e'], t['r'], t['\0']
 * As this could be an easier translation for each letter to build a string
 * to set names and things on the fly in the flipper. Need to explore that.
 * once I get to that point.
 */
#define TERM_		0x50
#define SPACE_		0x7f
#define A_		0x80
#define B_		0x81
#define C_		0x82
#define D_		0x83
#define E_		0x84
#define F_		0x85
#define G_		0x86
#define H_		0x87
#define I_		0x88
#define J_		0x89
#define K_		0x8a
#define L_		0x8b
#define M_		0x8c
#define N_		0x8d
#define O_		0x8e
#define P_		0x8f
#define Q_		0x90
#define R_		0x91
#define S_		0x92
#define T_		0x93
#define U_		0x94
#define V_		0x95
#define W_		0x96
#define X_		0x97
#define Y_		0x98
#define Z_		0x99
#define O_PAREN_	0x9a
#define C_PAREN_	0x9b
#define COLON_		0x9c
#define SEMI_		0x9d
#define O_BRACKET_	0x9e
#define C_BRACKET_	0x9f
#define a_		0xa0
#define b_		0xa1
#define c_		0xa2
#define d_		0xa3
#define e_		0xa4
#define f_		0xa5
#define g_		0xa6
#define h_		0xa7
#define i_		0xa8
#define j_		0xa9
#define k_		0xaa
#define l_		0xab
#define m_		0xac
#define n_		0xad
#define o_		0xae
#define p_		0xaf
#define q_		0xb0
#define r_		0xb1
#define s_		0xb2
#define t_		0xb3
#define u_		0xb4
#define v_		0xb5
#define w_		0xb6
#define x_		0xb7
#define y_		0xb8
#define z_		0xb9
#define e_ACCENT_	0xba
#define d_TICK_		0xbb
#define l_TICK_		0xbc
#define s_TICK_		0xbd
#define t_TICK_		0xbe
#define v_TICK_		0xbf
#define S_QUOTE_	0xe0
#define PK_		0xe1
#define MN_		0xe2
#define DASH_		0xe3
#define r_TICK_		0xe4
#define m_TICK_		0xe5
#define QUESTION_	0xe6
#define EXCLAIM_	0xe7
#define PERIOD_		0xe8
#define R_ARR_		0xec
#define D_ARR_		0xee
#define MALE_		0xef
#define FEMALE_		0xf5
#define HYPHEN_		0xe3
#define _0_		0xf6
#define _1_		0xf7
#define _2_		0xf8
#define _3_		0xf9
#define _4_		0xfa
#define _5_		0xfb
#define _6_		0xfc
#define _7_		0xfd
#define _8_		0xfe
#define _9_		0xff

char pokemon_char_to_encoded(int byte)
{
	switch(byte) {
	case 'A':		return A_;
	case 'B':		return B_;
	case 'C':		return C_;
	case 'D':		return D_;
	case 'E':		return E_;
	case 'F':		return F_;
	case 'G':		return G_;
	case 'H':		return H_;
	case 'I':		return I_;
	case 'J':		return J_;
	case 'K':		return K_;
	case 'L':		return L_;
	case 'M':		return M_;
	case 'N':		return N_;
	case 'O':		return O_;
	case 'P':		return P_;
	case 'Q':		return Q_;
	case 'R':		return R_;
	case 'S':		return S_;
	case 'T':		return T_;
	case 'U':		return U_;
	case 'V':		return V_;
	case 'W':		return W_;
	case 'X':		return X_;
	case 'Y':		return Y_;
	case 'Z':		return Z_;
	case 'a':		return a_;
	case 'b':		return b_;
	case 'c':		return c_;
	case 'd':		return d_;
	case 'e':		return e_;
	case 'f':		return f_;
	case 'g':		return g_;
	case 'h':		return h_;
	case 'i':		return i_;
	case 'j':		return j_;
	case 'k':		return k_;
	case 'l':		return l_;
	case 'm':		return m_;
	case 'n':		return n_;
	case 'o':		return o_;
	case 'p':		return p_;
	case 'q':		return q_;
	case 'r':		return r_;
	case 's':		return s_;
	case 't':		return t_;
	case 'u':		return u_;
	case 'v':		return v_;
	case 'w':		return w_;
	case 'x':		return x_;
	case 'y':		return y_;
	case 'z':		return z_;
	case '-':		return HYPHEN_;
	case '0':		return _0_;
	case '1':		return _1_;
	case '2':		return _2_;
	case '3':		return _3_;
	case '4':		return _4_;
	case '5':		return _5_;
	case '6':		return _6_;
	case '7':		return _7_;
	case '8':		return _8_;
	case '9':		return _9_;

	/* This was previously implemented with unicode escape codes, however, that
	 * seemed to cause compilation issues. Which is strange because others reported
	 * compilation issues with the actual unicode characters. I'm not sure a good
	 * universal way to resolve this.
	 *
	 * Additionally, the ♂/♀ symbols don't render properly on the flipper. Would
	 * need to create a custom image/icon somehow, otherwise its nonobvious that
	 * the traded pokemon would have this symbol in their name.
	 */

	case '\201':		return MALE_;
	case '\200':		return FEMALE_;
	default:		return TERM_;
	}
}

int pokemon_encoded_to_char(char byte)
{
	switch(byte) {
	case A_:		return 'A';
	case B_:		return 'B';
	case C_:		return 'C';
	case D_:		return 'D';
	case E_:		return 'E';
	case F_:		return 'F';
	case G_:		return 'G';
	case H_:		return 'H';
	case I_:		return 'I';
	case J_:		return 'J';
	case K_:		return 'K';
	case L_:		return 'L';
	case M_:		return 'M';
	case N_:		return 'N';
	case O_:		return 'O';
	case P_:		return 'P';
	case Q_:		return 'Q';
	case R_:		return 'R';
	case S_:		return 'S';
	case T_:		return 'T';
	case U_:		return 'U';
	case V_:		return 'V';
	case W_:		return 'W';
	case X_:		return 'X';
	case Y_:		return 'Y';
	case Z_:		return 'Z';
	case a_:		return 'a';
	case b_:		return 'b';
	case c_:		return 'c';
	case d_:		return 'd';
	case e_:		return 'e';
	case f_:		return 'f';
	case g_:		return 'g';
	case h_:		return 'h';
	case i_:		return 'i';
	case j_:		return 'j';
	case k_:		return 'k';
	case l_:		return 'l';
	case m_:		return 'm';
	case n_:		return 'n';
	case o_:		return 'o';
	case p_:		return 'p';
	case q_:		return 'q';
	case r_:		return 'r';
	case s_:		return 's';
	case t_:		return 't';
	case u_:		return 'u';
	case v_:		return 'v';
	case w_:		return 'w';
	case x_:		return 'x';
	case y_:		return 'y';
	case z_:		return 'z';
	case HYPHEN_:	return '-';
	case _0_:		return '0';
	case _1_:		return '1';
	case _2_:		return '2';
	case _3_:		return '3';
	case _4_:		return '4';
	case _5_:		return '5';
	case _6_:		return '6';
	case _7_:		return '7';
	case _8_:		return '8';
	case _9_:		return '9';

	/* This was previously implemented with unicode escape codes, however, that
	 * seemed to cause compilation issues. Which is strange because others reported
	 * compilation issues with the actual unicode characters. I'm not sure a good
	 * universal way to resolve this.
	 *
	 * Additionally, the ♂/♀ symbols don't render properly on the flipper. Would
	 * need to create a custom image/icon somehow, otherwise its nonobvious that
	 * the traded pokemon would have this symbol in their name.
	 */
	case MALE_:		return '\201';
	case FEMALE_:	return '\200';
	default:		return '\0';
	}
}

/* encode n bytes, any currently noninputtable characters are set with TERM_ */
void pokemon_str_to_encoded_array(uint8_t* dest, char* src, size_t n)
{
	for(; n > 0; n--) {
		*dest = pokemon_char_to_encoded(*src);
		dest++;
		src++;
	}
}

/* decode n bytes, any currently noninputtable characters are set with '\0' */
void pokemon_encoded_array_to_str(char* dest, uint8_t* src, size_t n)
{
	for(; n > 0; n--) {
		*dest = pokemon_encoded_to_char(*src);
		dest++;
		src++;
	}
}

void pokemon_encoded_array_clear(uint8_t* dest, size_t n)
{
	memset(dest, TERM_, n);
}
