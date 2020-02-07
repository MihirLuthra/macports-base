#if !defined(__RAND_STRING_GENERATOR_H__)
#define __RAND_STRING_GENERATOR_H__

/*
 * Descriptions:
 *  Allocated memory for a random strings whose characters range in between
 *  param3 & param4 and whose length is generated randomly between param1 & param2
 *
 * param1:
 *  Minimun possible length
 *
 * param2:
 *  Maximum possible length
 *
 * param3:
 *  Lower limit of possible characters
 *
 * param4:
 *  Upper limit of possible characters
 */
char * generate_rand_str(unsigned long long, unsigned long long, char, char);


/*
 * Description:
 *  Returns an array of randomly generated strings.
 *  Length of array is given by param1
 *  This functions calls generate_rand_str() to generate a random string
 *
 * param1:
 *  Length of array or num of random strings
 *
 * param2:
 *  Minimun possible length
 *
 * param3:
 *  Maximum possible length
 *
 * param4:
 *  Lower limit of possible characters
 *
 * param5:
 *  Upper limit of possible characters
 *
 */
char ** generate_rand_arr_of_strs(unsigned long long, unsigned long long, unsigned long long, char, char);

/*
 * Description:
 *  To free a string generated by generate_rand_str()
 *
 * param1:
 *  The string to be freed
 */
void free_rand_str(char *);

/*
 * Description:
 *  To free array of strings generated by generate_rand_arr_of_strs()
 *
 * param1:
 *  Array of strings
 *
 * param2:
 *  Number of strings
 */
void free_rand_strings(char **, unsigned long long);

#endif /* __RAND_STRING_GENERATOR_H__ */
